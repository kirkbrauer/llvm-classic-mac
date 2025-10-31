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
#include "RelocWriter.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "lld/Common/ErrorHandler.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/PEF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileOutputBuffer.h"
#include "llvm/Support/MathExtras.h"
#include <map>
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

// PEF Writer class (ImportedLibraryInfo now in RelocWriter.h)
class Writer {
public:
  Writer(std::vector<OutputSection *> sections) : outputSections(sections) {}

  void run();

private:
  void assignFileOffsets();
  void createLoaderSection();
  void collectImports();
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

  // Phase 2: Import tracking
  std::vector<ImportedLibraryInfo> importedLibraries;
  uint32_t totalImportedSymbolCount = 0;
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

void Writer::collectImports() {
  // Phase 2: Collect undefined symbols and group by library
  auto undefinedSymbols = symtab->getUndefinedSymbols();

  if (undefinedSymbols.empty()) {
    totalImportedSymbolCount = 0;
    return;
  }

  // Group undefined symbols by library
  // For now, default all imports to "InterfaceLib" (Mac OS Toolbox)
  // In a more complete implementation, we would:
  // 1. Check linker command line for library hints
  // 2. Look for shared library stubs (.shlib files)
  // 3. Use symbol name patterns to guess library

  std::map<std::string, std::vector<Undefined *>> libraryMap;

  for (Undefined *sym : undefinedSymbols) {
    // Default all undefined symbols to InterfaceLib
    // This matches CodeWarrior's behavior for Mac OS Toolbox functions
    libraryMap["InterfaceLib"].push_back(sym);
  }

  // Build ImportedLibraryInfo structures
  uint32_t currentImportIndex = 0;

  for (auto &pair : libraryMap) {
    ImportedLibraryInfo libInfo;
    // Note: pair.first is a std::string, need to keep it alive
    // For now, we know it's "InterfaceLib" which is a string literal
    libInfo.name = StringRef(pair.first);
    libInfo.symbols = std::move(pair.second);
    libInfo.firstImportedSymbol = currentImportIndex;

    currentImportIndex += libInfo.symbols.size();
    importedLibraries.push_back(std::move(libInfo));
  }

  totalImportedSymbolCount = currentImportIndex;
}

void Writer::createLoaderSection() {
  // Phase 2: Collect imports before building loader section
  collectImports();

  // Phase 3: Generate relocation instructions
  PEFRelocWriter relocWriter(outputSections, importedLibraries);
  auto [relocHeaders, relocInstrs] = relocWriter.generate();

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
    int16_t mainSection = def->getSectionIndex();
    uint32_t mainOffset = def->getValue();

    if (config->verbose) {
      errorHandler().outs() << "Entry point: " << config->entry
                           << " MainSection=" << mainSection
                           << " MainOffset=0x" << utohexstr(mainOffset) << "\n";
    }

    write32be(ptr + 0, mainSection);  // MainSection
    write32be(ptr + 4, mainOffset);   // MainOffset
  } else {
    write32be(ptr + 0, -1);  // No main
    write32be(ptr + 4, 0);
  }

  // InitSection, InitOffset, TermSection, TermOffset (all -1/0 for now)
  write32be(ptr + 8, -1);
  write32be(ptr + 12, 0);
  write32be(ptr + 16, -1);
  write32be(ptr + 20, 0);

  // ImportedLibraryCount, TotalImportedSymbolCount (Phase 2)
  write32be(ptr + 24, importedLibraries.size());
  write32be(ptr + 28, totalImportedSymbolCount);

  // Phase 3: RelocSectionCount and RelocInstrOffset
  uint32_t relocSectionCount = relocHeaders.size() / 12; // 12 bytes per header
  write32be(ptr + 32, relocSectionCount);

  // Calculate layout offsets
  uint32_t currentOffset = 56;  // After loader info header

  // ImportedLibrary structures (24 bytes each)
  currentOffset += importedLibraries.size() * 24;

  // ImportedSymbol table (4 bytes each)
  currentOffset += totalImportedSymbolCount * 4;

  // Phase 3: Relocation headers and instructions
  uint32_t relocInstrOffset = currentOffset;
  write32be(ptr + 36, relocInstrOffset);
  currentOffset += relocHeaders.size(); // Relocation headers
  currentOffset += relocInstrs.size();  // Relocation instructions

  // LoaderStringsOffset (after relocations)
  loaderStringsOffset = currentOffset;
  write32be(ptr + 40, loaderStringsOffset);

  // Build string table for both imported and exported symbols
  std::vector<uint8_t> stringTable;

  // Phase 2: Add imported library names and symbols to string table
  for (auto &lib : importedLibraries) {
    // Library name offset
    lib.nameOffset = stringTable.size();
    stringTable.insert(stringTable.end(), lib.name.begin(), lib.name.end());
    stringTable.push_back(0);  // Null terminator
  }

  // ImportedSymbol entries (store symbol info for later)
  struct ImportedSymbolEntry {
    uint32_t classAndName;
  };
  std::vector<ImportedSymbolEntry> importedSymbolEntries;

  for (auto &lib : importedLibraries) {
    for (Undefined *sym : lib.symbols) {
      ImportedSymbolEntry entry;
      uint32_t nameOffset = stringTable.size();
      StringRef name = sym->getName();
      stringTable.insert(stringTable.end(), name.begin(), name.end());
      stringTable.push_back(0);  // Null terminator

      // Build ImportedSymbol entry: 4 bits class + 28 bits name offset
      // Use TVector class for all imports (matches CodeWarrior)
      entry.classAndName = (static_cast<uint32_t>(PEF::kPEFTVectorSymbol) << 24) |
                          (nameOffset & 0x00FFFFFF);
      importedSymbolEntries.push_back(entry);
    }
  }

  // Build exported symbol entries
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

  // Phase 2: Write ImportedLibrary structures (24 bytes each)
  for (const auto &lib : importedLibraries) {
    uint8_t buf[24];
    write32be(buf + 0, lib.nameOffset);           // NameOffset
    write32be(buf + 4, 0);                         // OldImpVersion
    write32be(buf + 8, 0);                         // CurrentVersion
    write32be(buf + 12, lib.symbols.size());       // ImportedSymbolCount
    write32be(buf + 16, lib.firstImportedSymbol);  // FirstImportedSymbol
    write8(buf + 20, 0);                           // Options (0 = strong imports)
    write8(buf + 21, 0);                           // ReservedA
    write16be(buf + 22, 0);                        // ReservedB
    loaderData.insert(loaderData.end(), buf, buf + 24);
  }

  // Phase 2: Write ImportedSymbol table (4 bytes each)
  for (const auto &entry : importedSymbolEntries) {
    uint8_t buf[4];
    write32be(buf, entry.classAndName);
    loaderData.insert(loaderData.end(), buf, buf + 4);
  }

  // Phase 3: Write relocation headers and instructions
  loaderData.insert(loaderData.end(), relocHeaders.begin(), relocHeaders.end());
  loaderData.insert(loaderData.end(), relocInstrs.begin(), relocInstrs.end());

  // Write string table
  loaderData.insert(loaderData.end(), stringTable.begin(), stringTable.end());

  // Align to hash table offset
  while (loaderData.size() < exportHashOffset)
    loaderData.push_back(0);

  // Write hash table (2^exportHashTablePower slots, 4 bytes each)
  // With power=0, we have 1 slot
  uint32_t hashSlotCount = 1u << 0; // ExportHashTablePower = 0
  for (uint32_t i = 0; i < hashSlotCount; ++i) {
    uint8_t buf[4];
    write32be(buf, 0xFFFFFFFF); // Empty slot marker
    loaderData.insert(loaderData.end(), buf, buf + 4);
  }

  // Write key table (one 4-byte entry per exported symbol)
  // Each entry is the full hash of the symbol name, used for lookup
  for (uint32_t i = 0; i < exportedSymbolCount; ++i) {
    uint8_t buf[4];
    write32be(buf, i); // Simple ascending keys for now
    loaderData.insert(loaderData.end(), buf, buf + 4);
  }

  // Write exported symbols (after hash and key tables)
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
    // Code sections use Global share (matches CodeWarrior), data sections use Process share
    uint8_t shareKind = (osec->getKind() == PEF::kPEFCodeSection) ?
                        PEF::kPEFGlobalShare : PEF::kPEFProcessShare;
    write8(buf + 25, shareKind);                    // ShareKind
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
