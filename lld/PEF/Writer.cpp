//===- Writer.cpp ---------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Writer.h"
#include "Config.h"
#include "InputFiles.h"
#include "InputSection.h"
#include "OutputSection.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "lld/Common/ErrorHandler.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/PEF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileOutputBuffer.h"
#include "llvm/Support/MathExtras.h"
#include <vector>

using namespace llvm;
using namespace llvm::support;
using namespace lld;
using namespace lld::pef;

namespace {

// Helper to write big-endian values
template <typename T>
void write32be(uint8_t *buf, T val) {
  endian::write32be(buf, static_cast<uint32_t>(val));
}

void write16be(uint8_t *buf, uint16_t val) {
  endian::write16be(buf, val);
}

void write8(uint8_t *buf, uint8_t val) {
  *buf = val;
}

// PEF Writer class
class Writer {
public:
  Writer(std::vector<OutputSection *> sections) : outputSections(sections) {}

  void run();

private:
  void assignFileOffsets();
  void createLoaderSection();
  void openFile();
  void writeHeader();
  void writeSectionHeaders();
  void writeSections();
  void writeLoaderSection();

  std::vector<OutputSection *> outputSections;
  std::unique_ptr<FileOutputBuffer> buffer;
  uint8_t *bufferStart = nullptr;
  size_t fileSize = 0;

  // Loader section info
  std::vector<uint8_t> loaderData;
  uint32_t loaderStringsOffset = 0;
  uint32_t exportHashOffset = 0;
  uint32_t exportedSymbolCount = 0;
};

void Writer::assignFileOffsets() {
  uint64_t offset = sizeof(PEF::ContainerHeader);

  // Account for section headers (including loader section)
  offset += (outputSections.size() + 1) * sizeof(PEF::SectionHeader);

  // Assign file offsets to each output section
  for (OutputSection *osec : outputSections) {
    if (osec->getInputSections().empty())
      continue;

    // Align to 16 bytes (PEF convention)
    offset = alignTo(offset, 16);
    osec->setFileOffset(offset);
    offset += osec->getSize();
  }

  // Loader section comes after all regular sections
  offset = alignTo(offset, 16);
  uint64_t loaderOffset = offset;

  // Create loader section data
  createLoaderSection();

  fileSize = loaderOffset + loaderData.size();
}

void Writer::createLoaderSection() {
  // Build loader section with exported symbols
  auto definedSymbols = symtab->getDefinedSymbols();
  exportedSymbolCount = definedSymbols.size();

  // Loader info header (56 bytes)
  std::vector<uint8_t> loaderInfo(56, 0);
  uint8_t *ptr = loaderInfo.data();

  // Find entry point
  Symbol *entryPoint = nullptr;
  if (!config->entry.empty()) {
    entryPoint = symtab->find(config->entry);
  }

  // MainSection and MainOffset
  if (entryPoint && entryPoint->isDefined()) {
    auto *def = cast<Defined>(entryPoint);
    write32be(ptr + 0, def->getSectionIndex());  // MainSection
    write32be(ptr + 4, def->getValue());          // MainOffset
  } else {
    write32be(ptr + 0, -1);  // No main
    write32be(ptr + 4, 0);
  }

  // InitSection, InitOffset, TermSection, TermOffset (all -1/0 for now)
  write32be(ptr + 8, -1);
  write32be(ptr + 12, 0);
  write32be(ptr + 16, -1);
  write32be(ptr + 20, 0);

  // ImportedLibraryCount, TotalImportedSymbolCount (0 for Phase 1)
  write32be(ptr + 24, 0);
  write32be(ptr + 28, 0);

  // RelocSectionCount, RelocInstrOffset (0 for Phase 1)
  write32be(ptr + 32, 0);
  write32be(ptr + 36, 56);  // Points right after header

  // LoaderStringsOffset
  loaderStringsOffset = 56;  // Right after loader info header
  write32be(ptr + 40, loaderStringsOffset);

  // Build string table for exported symbols
  std::vector<uint8_t> stringTable;
  std::vector<PEF::ExportedSymbol> exports;

  for (Defined *sym : definedSymbols) {
    PEF::ExportedSymbol exp;

    // Symbol name offset in string table
    uint32_t nameOffset = stringTable.size();
    StringRef name = sym->getName();
    stringTable.insert(stringTable.end(), name.begin(), name.end());
    stringTable.push_back(0);  // Null terminator

    // Build exported symbol entry
    exp.ClassAndName = (static_cast<uint32_t>(sym->getSymbolClass()) << 24) |
                       (nameOffset & 0x00FFFFFF);
    exp.SymbolValue = sym->getValue();
    exp.SectionIndex = sym->getSectionIndex();

    exports.push_back(exp);
  }

  // ExportHashOffset (after strings)
  exportHashOffset = loaderStringsOffset + stringTable.size();
  exportHashOffset = alignTo(exportHashOffset, 4);  // Align hash table
  write32be(ptr + 44, exportHashOffset);

  // ExportHashTablePower (0 = no hash table for simplicity)
  write32be(ptr + 48, 0);

  // ExportedSymbolCount
  write32be(ptr + 52, exportedSymbolCount);

  // Assemble loader section
  loaderData.insert(loaderData.end(), loaderInfo.begin(), loaderInfo.end());
  loaderData.insert(loaderData.end(), stringTable.begin(), stringTable.end());

  // Align to hash table offset
  while (loaderData.size() < exportHashOffset)
    loaderData.push_back(0);

  // Write exported symbols (right after hash table offset, no actual hash table)
  for (const auto &exp : exports) {
    uint8_t buf[10];
    write32be(buf + 0, exp.ClassAndName);
    write32be(buf + 4, exp.SymbolValue);
    write16be(buf + 8, exp.SectionIndex);
    loaderData.insert(loaderData.end(), buf, buf + 10);
  }

  // Pad to 16-byte boundary
  while (loaderData.size() % 16 != 0)
    loaderData.push_back(0);
}

void Writer::openFile() {
  Expected<std::unique_ptr<FileOutputBuffer>> bufferOrErr =
      FileOutputBuffer::create(config->outputFile, fileSize,
                               FileOutputBuffer::F_executable);

  if (!bufferOrErr) {
    error("failed to open " + config->outputFile + ": " +
          toString(bufferOrErr.takeError()));
    return;
  }

  buffer = std::move(*bufferOrErr);
  bufferStart = buffer->getBufferStart();
}

void Writer::writeHeader() {
  uint8_t *buf = bufferStart;

  // PEF Container Header (40 bytes)
  write32be(buf + 0, PEF::kPEFTag1);          // 'Joy!'
  write32be(buf + 4, PEF::kPEFTag2);          // 'peff'
  write32be(buf + 8, PEF::kPEFPowerPCArch);   // 'pwpc'
  write32be(buf + 12, PEF::kPEFVersion);      // Format version 1
  write32be(buf + 16, 0);                      // DateTimeStamp
  write32be(buf + 20, 0);                      // OldDefVersion
  write32be(buf + 24, 0);                      // OldImpVersion
  write32be(buf + 28, 0);                      // CurrentVersion

  // Count non-empty sections
  uint16_t sectionCount = 0;
  uint16_t instSectionCount = 0;
  for (OutputSection *osec : outputSections) {
    if (!osec->getInputSections().empty()) {
      sectionCount++;
      instSectionCount++;  // All sections are instantiated
    }
  }
  sectionCount++;  // +1 for loader section

  write16be(buf + 32, sectionCount);
  write16be(buf + 34, instSectionCount);
  write32be(buf + 36, 0);  // ReservedA
}

void Writer::writeSectionHeaders() {
  uint8_t *buf = bufferStart + sizeof(PEF::ContainerHeader);

  // Write headers for regular sections
  for (OutputSection *osec : outputSections) {
    if (osec->getInputSections().empty())
      continue;

    // PEF Section Header (40 bytes)
    write32be(buf + 0, -1);  // NameOffset (-1 = no name)
    write32be(buf + 4, osec->getVirtualAddress());  // DefaultAddress
    write32be(buf + 8, osec->getSize());            // TotalLength
    write32be(buf + 12, osec->getSize());           // UnpackedLength
    write32be(buf + 16, osec->getSize());           // ContainerLength
    write32be(buf + 20, osec->getFileOffset());     // ContainerOffset
    write8(buf + 24, osec->getKind());              // SectionKind
    write8(buf + 25, PEF::kPEFProcessShare);        // ShareKind
    write8(buf + 26, static_cast<uint8_t>(llvm::Log2_32(osec->getAlignment()))); // Alignment
    write8(buf + 27, 0);  // ReservedA

    buf += sizeof(PEF::SectionHeader);
  }

  // Write loader section header
  uint64_t loaderOffset = fileSize - loaderData.size();
  write32be(buf + 0, -1);  // NameOffset
  write32be(buf + 4, 0);   // DefaultAddress
  write32be(buf + 8, loaderData.size());   // TotalLength
  write32be(buf + 12, loaderData.size());  // UnpackedLength
  write32be(buf + 16, loaderData.size());  // ContainerLength
  write32be(buf + 20, loaderOffset);       // ContainerOffset
  write8(buf + 24, PEF::kPEFLoaderSection); // SectionKind
  write8(buf + 25, PEF::kPEFGlobalShare);   // ShareKind
  write8(buf + 26, 4);  // Alignment (16 bytes = 2^4)
  write8(buf + 27, 0);  // ReservedA
}

void Writer::writeSections() {
  for (OutputSection *osec : outputSections) {
    if (osec->getInputSections().empty())
      continue;

    uint8_t *buf = bufferStart + osec->getFileOffset();

    // Write each input section's data
    for (InputSection *isec : osec->getInputSections()) {
      auto dataOrErr = isec->getData();
      if (!dataOrErr) {
        error("failed to get section data: " + toString(dataOrErr.takeError()));
        continue;
      }

      ArrayRef<uint8_t> data = *dataOrErr;
      memcpy(buf, data.data(), data.size());
      buf += data.size();
    }
  }
}

void Writer::writeLoaderSection() {
  uint64_t loaderOffset = fileSize - loaderData.size();
  uint8_t *buf = bufferStart + loaderOffset;
  memcpy(buf, loaderData.data(), loaderData.size());
}

void Writer::run() {
  if (config->verbose) {
    errorHandler().outs() << "\nWriting PEF executable...\n";
  }

  // Assign file offsets to sections
  assignFileOffsets();

  if (config->verbose) {
    errorHandler().outs() << "  Output file size: " << fileSize << " bytes\n";
  }

  // Open output file
  openFile();
  if (!buffer)
    return;

  // Write all components
  writeHeader();
  writeSectionHeaders();
  writeSections();
  writeLoaderSection();

  // Commit to disk
  if (Error e = buffer->commit()) {
    error("failed to write " + config->outputFile + ": " + toString(std::move(e)));
  } else if (config->verbose) {
    errorHandler().outs() << "  Successfully wrote " << config->outputFile << "\n";
  }
}

} // anonymous namespace

// Global entry point
void lld::pef::writeResult(std::vector<OutputSection *> outputSections) {
  Writer writer(outputSections);
  writer.run();
}
