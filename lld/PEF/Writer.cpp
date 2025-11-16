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
#include "PatternEncoder.h"
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
#include <ctime>

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
  void createEntryPointTVect();
  void updateEntryPointTVect();  // Update TVect TOC address after collectImports
  void generateImportStubs();  // Generate stubs in code section
  void generateTOCEntries();   // Generate TOC entries in data section
  void replaceImportCalls();  // Replace bl .+1 with calls to import stubs
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
  uint64_t loaderSectionOffset = 0;  // File offset where loader section starts
  uint32_t loaderStringsOffset = 0;
  uint32_t exportHashOffset = 0;
  uint32_t exportedSymbolCount = 0;

  // Phase 2: Import tracking
  std::vector<ImportedLibraryInfo> importedLibraries;
  uint32_t totalImportedSymbolCount = 0;

  // Entry point TVect location and data
  int16_t tvectSectionIndex = -1;
  uint32_t tvectOffset = 0;
  std::vector<uint8_t> tvectData;

  // Standard PEF import implementation
  // TOC entries in data section (12 bytes each: function_ptr, toc_value, reserved)
  uint32_t tocEntriesOffset = 0;     // Offset in data section where TOC entries start
  uint32_t tocEntriesSize = 0;       // Total size of TOC entries (imports * 12)

  // Helper function to check if a section is a data section (PIData or UnpackedData)
  bool isDataSection(uint8_t kind) {
    return kind == PEF::kPEFPatternDataSection || kind == PEF::kPEFUnpackedDataSection;
  }

  // BUG FIX #35: Import stubs in code section (24 bytes each, matching CodeWarrior)
  std::vector<uint8_t> importStubs;  // Buffer containing all import stubs
  std::map<Symbol*, uint32_t> stubOffsets;  // Map symbol to stub offset in code section

  void assignSymbolAddresses();  // Assign virtual addresses to defined symbols
  void assignImportAddresses();

  // Patched code storage for bl .+1 replacement
  std::map<InputSection*, std::vector<uint8_t>> patchedCode;

  // Import-related data
  std::vector<ImportedSymbol*> importedSymbols;
};

void Writer::assignFileOffsets() {
  uint64_t offset = sizeof(PEF::ContainerHeader);

  // Count non-empty sections to allocate correct header space
  uint16_t nonEmptySections = 0;
  for (size_t i = 0; i < outputSections.size(); ++i) {
    OutputSection *osec = outputSections[i];
    bool hasTVect = (tvectSectionIndex >= 0 && static_cast<int16_t>(i) == tvectSectionIndex && !tvectData.empty());
    if (!osec->getInputSections().empty() || hasTVect)
      nonEmptySections++;
  }

  if (config->verbose) {
    errorHandler().outs() << "nonEmptySections=" << nonEmptySections
                         << " outputSections.size()=" << outputSections.size() << "\n";
    errorHandler().outs() << "Initial offset (container header): 0x" << utohexstr(offset) << "\n";
    errorHandler().outs() << "kSectionHeaderFileSize: " << PEF::kSectionHeaderFileSize << "\n";
    errorHandler().outs() << "(nonEmptySections + 1): " << (nonEmptySections + 1) << "\n";
    errorHandler().outs() << "Header space to add: " << ((nonEmptySections + 1) * PEF::kSectionHeaderFileSize) << "\n";
  }

  // Account for section headers (only non-empty + loader)
  // kSectionHeaderFileSize is 28 bytes per Apple PEF specification
  offset += (nonEmptySections + 1) * PEF::kSectionHeaderFileSize;

  if (config->verbose) {
    errorHandler().outs() << "Offset after adding headers: 0x" << utohexstr(offset) << "\n";
    errorHandler().outs() << "Header space allocated: " << ((nonEmptySections + 1) * PEF::kSectionHeaderFileSize)
                         << " bytes, first section starts at 0x" << utohexstr(offset) << "\n";
  }

  // Assign file offsets to each non-empty output section
  for (size_t i = 0; i < outputSections.size(); ++i) {
    OutputSection *osec = outputSections[i];
    // BUG FIX #13: Don't skip section if it contains TVect, even if no input sections
    bool hasTVect = (tvectSectionIndex >= 0 && static_cast<int16_t>(i) == tvectSectionIndex && !tvectData.empty());
    if (osec->getInputSections().empty() && !hasTVect)
      continue;  // Skip empty sections (no file offset needed)

    // Align to 16 bytes (PEF convention)
    offset = alignTo(offset, 16);
    osec->setFileOffset(offset);

    uint64_t sectionSize = osec->getSize();

    // Save the original size for later reference
    osec->setOriginalSize(sectionSize);

    // If this is the data section, reserve space for import table (CodeWarrior model)
    if (isDataSection(osec->getKind())) {
      // Reserve space for import address table (4 bytes per import)
      uint32_t importTableSize = totalImportedSymbolCount * 4;

      // CodeWarrior model: NO TOC entries, use direct import table access
      // r2 points to start of data section, stubs load directly from import table
      tocEntriesSize = 0;
      tocEntriesOffset = 0;

      // Data section layout: [Import table][TVect]
      // For minimal test with 1 import: 4 + 12 = 16 bytes
      sectionSize += importTableSize + 12;  // Only import table + TVect

      if (config->verbose && importTableSize > 0) {
        errorHandler().outs() << "Data section additions (CodeWarrior model):\n"
                             << "  Import table: " << importTableSize << " bytes\n"
                             << "  TVect: 12 bytes\n"
                             << "  Total: " << (importTableSize + 12) << " bytes\n"
                             << "  (No TOC entries - using direct import access)\n";
      }
    }

    // If this is the code section, reserve space for import stubs
    if (osec->getKind() == PEF::kPEFCodeSection && totalImportedSymbolCount > 0) {
      // BUG FIX #35: Import stubs are 24 bytes (matching CodeWarrior)
      // Reserve space for import stubs (24 bytes per import)
      uint32_t stubsSize = totalImportedSymbolCount * 24;
      sectionSize += stubsSize;

      if (config->verbose) {
        errorHandler().outs() << "Code section additions:\n"
                             << "  Import stubs: " << stubsSize << " bytes ("
                             << totalImportedSymbolCount << " imports × 24 bytes)\n";
      }
    }

    // BUG FIX #24: TVect size is now included in the data section calculation above (line 148)
    // Do NOT add it separately here - that would cause duplicate counting!

    osec->setSize(sectionSize);

    // For data sections, perform pattern encoding now to determine file size
    uint64_t sizeInFile = sectionSize;
    if (isDataSection(osec->getKind())) {
      // Prepare data for pattern encoding
      std::vector<uint8_t> dataContent;

      uint32_t importTableSize = totalImportedSymbolCount * 4;
      dataContent.insert(dataContent.end(), importTableSize, 0);

      // TVect (12 bytes) - at this point tvectData should exist from createEntryPointTVect()
      if (tvectSectionIndex >= 0 && static_cast<int16_t>(i) == tvectSectionIndex &&
          !tvectData.empty()) {
        dataContent.insert(dataContent.end(), tvectData.begin(), tvectData.end());
      }

      // Encode now so we know the file size
      std::vector<uint8_t> encoded = PatternEncoder::encode(dataContent);
      osec->setEncodedData(encoded);
      osec->setUnpackedLength(dataContent.size());

      // Use encoded size for file offset calculation
      sizeInFile = encoded.size();

      if (config->verbose) {
        errorHandler().outs() << "Data section pattern encoding:\n"
                             << "  Memory size: " << sectionSize << " bytes\n"
                             << "  Unpacked size: " << dataContent.size() << " bytes\n"
                             << "  File size: " << sizeInFile << " bytes\n";
      }
    }

    if (config->verbose) {
      const char *kindName = "unknown";
      if (osec->getKind() == PEF::kPEFCodeSection) kindName = "code";
      else if (isDataSection(osec->getKind())) kindName = "data";
      errorHandler().outs() << "Section " << i << " (" << kindName
                           << "): fileOffset=0x" << utohexstr(osec->getFileOffset())
                           << ", size=" << sectionSize
                           << ", file size=" << sizeInFile << "\n";
    }

    offset += sizeInFile;
  }

  // Loader section comes after all regular sections
  offset = alignTo(offset, 16);
  loaderSectionOffset = offset;

  // BUG FIX #15: Don't create loader section here - it needs tvectOffset to be finalized
  // We'll create it later in run() after updateEntryPointTVect()
  // For now, just reserve space (we'll calculate actual size later)
  // Estimate loader size (will be updated when actually created)
  uint32_t estimatedLoaderSize = 256;  // Conservative estimate
  fileSize = offset + estimatedLoaderSize;
}

void Writer::assignSymbolAddresses() {
  // Assign virtual addresses to all defined symbols
  // Virtual address = section base address + symbol offset

  if (config->verbose) {
    errorHandler().outs() << "\nAssigning virtual addresses to symbols:\n";
  }

  for (size_t secIdx = 0; secIdx < outputSections.size(); ++secIdx) {
    OutputSection *osec = outputSections[secIdx];
    uint64_t sectionBase = osec->getVirtualAddress();

    // Get all defined symbols from the symbol table
    std::vector<Defined*> allDefined = symtab->getDefinedSymbols();

    for (Defined *sym : allDefined) {
      // Only process symbols in this section
      if (sym->getSectionIndex() != static_cast<int16_t>(secIdx))
        continue;

      // Calculate virtual address: section base + symbol offset
      uint64_t virtualAddr = sectionBase + sym->getValue();
      sym->setVirtualAddress(virtualAddr);

      if (config->verbose) {
        errorHandler().outs() << "  Symbol '" << sym->getName()
                             << "' section=" << secIdx
                             << " offset=0x" << utohexstr(sym->getValue())
                             << " virtualAddr=0x" << utohexstr(virtualAddr) << "\n";
      }
    }
  }
}

void Writer::assignImportAddresses() {
  // Assign virtual addresses to each imported symbol
  // These addresses are where CFM will patch transition vector pointers

  if (importedLibraries.empty()) {
    return;
  }

  // Import table starts at the beginning of the data section
  uint32_t offset = 0;

  for (auto &lib : importedLibraries) {
    for (size_t i = 0; i < lib.symbols.size(); i++) {
      ImportedSymbol *sym = lib.symbols[i];
      uint32_t globalIndex = lib.firstImportedSymbol + i;

      // Virtual address in data section where CFM will patch this import
      // This is section-relative (offset within data section)
      sym->setVirtualAddress(offset + (globalIndex * 4));

      if (config->verbose) {
        errorHandler().outs() << "Import " << sym->getName()
                             << " index=" << globalIndex
                             << " address=0x" << utohexstr(sym->getVirtualAddress()) << "\n";
      }
    }
  }
}

void Writer::collectImports() {
  // Phase 2: Collect imported symbols and group by library
  importedSymbols = symtab->getImportedSymbols();

  if (importedSymbols.empty()) {
    totalImportedSymbolCount = 0;
    return;
  }

  // Group imported symbols by library
  // ImportedSymbol objects already know which library they come from
  // IMPORTANT: Use vector of pairs to preserve insertion order (not alphabetical)
  // CodeWarrior orders imports by first encounter, not alphabetically
  std::vector<std::pair<StringRef, std::vector<ImportedSymbol *>>> libraryMap;

  for (ImportedSymbol *sym : importedSymbols) {
    // Extract library name from the ImportedSymbol
    StringRef libName = sym->getLibrary()->getLibraryName();

    // Find existing library entry or create new one (preserves insertion order)
    bool found = false;
    for (auto &pair : libraryMap) {
      if (pair.first == libName) {
        pair.second.push_back(sym);
        found = true;
        break;
      }
    }
    if (!found) {
      libraryMap.push_back({libName, {sym}});
    }
  }

  // Build ImportedLibraryInfo structures
  uint32_t currentImportIndex = 0;

  for (auto &pair : libraryMap) {
    ImportedLibraryInfo libInfo;
    libInfo.name = pair.first;
    libInfo.symbols = std::move(pair.second);
    libInfo.firstImportedSymbol = currentImportIndex;

    currentImportIndex += libInfo.symbols.size();
    importedLibraries.push_back(std::move(libInfo));
  }

  totalImportedSymbolCount = currentImportIndex;
}

void Writer::createLoaderSection() {
  // Phase 3: Generate relocation instructions
  // Note: collectImports() is now called earlier in run() before assignFileOffsets()
  PEFRelocWriter relocWriter(outputSections, importedLibraries);
  auto [relocHeaders, relocInstrs] = relocWriter.generate();

  // Build loader section with exported symbols
  // For executables, only export symbols if --export-dynamic is specified
  // (matches behavior of CodeWarrior and Retro68 which don't export from executables)
  std::vector<Defined *> definedSymbols;
  if (config->exportDynamic) {
    // Export all defined symbols (for debugging or if explicitly requested)
    definedSymbols = symtab->getDefinedSymbols();
  }
  // Otherwise, executables have empty export table (standard behavior)

  exportedSymbolCount = definedSymbols.size();

  // Loader info header (56 bytes)
  std::vector<uint8_t> loaderInfo(56, 0);
  uint8_t *ptr = loaderInfo.data();

  // BUG FIX #21: REVERT BUG #18 - MainSection/MainOffset MUST point to TVect in data section
  // CFM requires entry point to be a TVect descriptor, not raw code
  // TVect structure: [code_address, toc_address, environment]
  // CFM reads the TVect, sets r2 from toc_address, then jumps to code_address
  if (tvectSectionIndex >= 0) {
    // Entry point is the TVect descriptor in data section
    if (config->verbose) {
      errorHandler().outs() << "Entry point TVect: " << config->entry
                           << " MainSection=" << tvectSectionIndex
                           << " MainOffset=0x" << utohexstr(tvectOffset) << "\n";
    }

    write32be(ptr + 0, tvectSectionIndex);  // MainSection (data section with TVect)
    write32be(ptr + 4, tvectOffset);        // MainOffset (offset of TVect in data)
  } else {
    // Fallback: no TVect created (shouldn't happen for normal executables)
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
  // RelocInstrOffset must point to the instructions, AFTER the headers
  currentOffset += relocHeaders.size(); // Relocation headers come first
  uint32_t relocInstrOffset = currentOffset;  // Instructions start after headers
  write32be(ptr + 36, relocInstrOffset);
  currentOffset += relocInstrs.size();  // Relocation instructions

  // LoaderStringsOffset (after relocations)
  loaderStringsOffset = currentOffset;
  write32be(ptr + 40, loaderStringsOffset);

  // Build string table for both imported and exported symbols
  std::vector<uint8_t> stringTable;

  // Phase 2: Add imported library names and symbols to string table
  // PEF specification requires null-terminated C strings for imported libraries
  for (auto &lib : importedLibraries) {
    // Library name offset
    lib.nameOffset = stringTable.size();
    stringTable.insert(stringTable.end(), lib.name.begin(), lib.name.end());
    stringTable.push_back('\0');  // Null terminator (required by PEF spec)
  }

  // ImportedSymbol entries (store symbol info for later)
  struct ImportedSymbolEntry {
    uint32_t classAndName;
  };
  std::vector<ImportedSymbolEntry> importedSymbolEntries;

  for (auto &lib : importedLibraries) {
    for (ImportedSymbol *sym : lib.symbols) {
      ImportedSymbolEntry entry;
      uint32_t nameOffset = stringTable.size();
      StringRef name = sym->getName();
      // PEF specification requires null-terminated C strings for imported symbols
      stringTable.insert(stringTable.end(), name.begin(), name.end());
      stringTable.push_back('\0');  // Null terminator (required by PEF spec)

      // Build ImportedSymbol entry: 4 bits class + 24 bits name offset
      // Mac OS 9 CFM uses bits 27-24 for class, bits 23-0 for offset
      // Use the symbol class from the ImportedSymbol (typically kPEFTVectorSymbol)
      entry.classAndName = (static_cast<uint32_t>(sym->getSymbolClass()) << 24) |
                          (nameOffset & 0x00FFFFFF);
      importedSymbolEntries.push_back(entry);

      if (config->verbose) {
        errorHandler().outs() << "Import symbol: " << name
                             << ", class=0x" << utohexstr(sym->getSymbolClass())
                             << ", nameOffset=0x" << utohexstr(nameOffset)
                             << ", classAndName=0x" << utohexstr(entry.classAndName) << "\n";
      }
    }
  }

  // Build exported symbol entries
  std::vector<PEF::ExportedSymbol> exports;

  for (Defined *sym : definedSymbols) {
    PEF::ExportedSymbol exp;

    // Symbol name offset in string table
    uint32_t nameOffset = stringTable.size();
    StringRef name = sym->getName();

    // PEF spec: exported symbols don't require null termination, but we add it for consistency
    stringTable.insert(stringTable.end(), name.begin(), name.end());
    stringTable.push_back('\0');  // Null terminator

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

  // BUG FIX #28: ExportHashTablePower must be 1 to match CodeWarrior
  // Even with 0 exports, CFM may validate this field (power=1 means 2 slots)
  write32be(ptr + 48, 1);

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
    if (config->verbose) {
      errorHandler().outs() << "Writing import symbol entry: classAndName=0x"
                           << utohexstr(entry.classAndName)
                           << " bytes=[" << utohexstr(buf[0]) << " " << utohexstr(buf[1])
                           << " " << utohexstr(buf[2]) << " " << utohexstr(buf[3]) << "]\n";
    }
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
  // BUG FIX #28: With power=1, we have 2 slots (matching CodeWarrior)
  uint32_t hashSlotCount = 1u << 1; // ExportHashTablePower = 1
  for (uint32_t i = 0; i < hashSlotCount; ++i) {
    uint8_t buf[4];
    write32be(buf, 0x00000000); // Empty hash slot (Mac OS 9 requires 0x00000000)
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

  // NOTE: Loader section data does NOT need to be 16-byte aligned.
  // Only the containerOffset (file position) needs 16-byte alignment.
  // CodeWarrior does not pad loader data, so we don't either.
  // The section header's alignment field (16 bytes) applies to file placement only.
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

  // Generate valid Macintosh timestamp (seconds since Jan 1, 1904)
  // Mac epoch is 2,082,844,800 seconds before Unix epoch (Jan 1, 1970)
  uint32_t macTimestamp = (uint32_t)std::time(nullptr) + 2082844800UL;
  write32be(buf + 16, macTimestamp);           // DateTimeStamp (Mac epoch)

  write32be(buf + 20, 0);                      // OldDefVersion
  write32be(buf + 24, 0);                      // OldImpVersion
  write32be(buf + 28, 0);                      // CurrentVersion

  // Count only non-empty sections (matches headers actually written)
  uint16_t sectionCount = 0;
  uint16_t instSectionCount = 0;
  for (size_t i = 0; i < outputSections.size(); ++i) {
    OutputSection *osec = outputSections[i];
    // BUG FIX #13: Count section if it has TVect, even if no input sections
    bool hasTVect = (tvectSectionIndex >= 0 && static_cast<int16_t>(i) == tvectSectionIndex && !tvectData.empty());
    if (!osec->getInputSections().empty() || hasTVect) {
      sectionCount++;       // Count non-empty sections
      instSectionCount++;   // All non-empty are instantiated
    }
  }
  sectionCount++;  // +1 for loader section

  write16be(buf + 32, sectionCount);
  write16be(buf + 34, instSectionCount);
  write32be(buf + 36, 0);  // ReservedA
}

void Writer::writeSectionHeaders() {
  uint8_t *buf = bufferStart + sizeof(PEF::ContainerHeader);

  // Write headers only for non-empty sections
  for (size_t i = 0; i < outputSections.size(); ++i) {
    OutputSection *osec = outputSections[i];
    // BUG FIX #13: Don't skip section if it contains TVect, even if no input sections
    bool hasTVect = (tvectSectionIndex >= 0 && static_cast<int16_t>(i) == tvectSectionIndex && !tvectData.empty());

    // Skip empty sections - don't write headers for them
    if (osec->getInputSections().empty() && !hasTVect)
      continue;

    // BUG FIX #24: Don't add TVect size here - it's already included in osec->getSize()
    // from the addition in assignFileOffsets(). Adding it again causes section overlap!
    uint64_t sectionSize = osec->getSize();
    uint64_t fileOffset = osec->getFileOffset();

    if (isDataSection(osec->getKind())) {
      errorHandler().log("DEBUG writeSectionHeaders: read data section size as " + std::to_string(sectionSize));
    }

    // PEF Section Header (28 bytes per Apple specification)
    write32be(buf + 0, -1);  // NameOffset (-1 = no name)
    write32be(buf + 4, 0);  // DefaultAddress (0 = anywhere)

    // Check if this section uses pattern-init encoding
    if (osec->hasEncodedData()) {
      // Pattern-initialized data section
      uint32_t totalLength = osec->getSize();  // Total size in memory
      uint32_t unpackedLength = osec->getUnpackedLength();  // Size of pattern data
      uint32_t containerLength = osec->getEncodedData().size();  // Encoded size in file

      write32be(buf + 8, totalLength);       // TotalLength
      write32be(buf + 12, unpackedLength);   // UnpackedLength (12 for minimal test)
      write32be(buf + 16, containerLength);  // ContainerLength (1 byte for 0x0C)
      write32be(buf + 20, fileOffset);       // ContainerOffset

      if (config->verbose) {
        errorHandler().outs() << "Writing pattern-init section header " << i
                             << ": TotalLength=" << totalLength
                             << ", UnpackedLength=" << unpackedLength
                             << ", ContainerLength=" << containerLength
                             << ", Offset=0x" << utohexstr(fileOffset) << "\n";
      }

      write8(buf + 24, PEF::kPEFPatternDataSection);  // SectionKind = 2
    } else {
      // Regular unpacked section
      write32be(buf + 8, sectionSize);       // TotalLength
      write32be(buf + 12, sectionSize);      // UnpackedLength
      write32be(buf + 16, sectionSize);      // ContainerLength
      write32be(buf + 20, fileOffset);       // ContainerOffset

      if (config->verbose) {
        const char *kindName = "unknown";
        if (osec->getKind() == PEF::kPEFCodeSection) kindName = "code";
        else if (isDataSection(osec->getKind())) kindName = "data";
        errorHandler().outs() << "Writing section header " << i << " (" << kindName
                             << "): ContainerOffset=0x" << utohexstr(fileOffset)
                             << ", ContainerLength=" << sectionSize << "\n";
      }

      write8(buf + 24, osec->getKind());  // SectionKind
    }

    // ShareKind and Alignment (same for both types)
    // BUG FIX: Data sections should use Process share, not Global
    // CodeWarrior uses Process(1) for data, Global(4) for code
    uint8_t shareKind = (osec->getKind() == PEF::kPEFCodeSection) ?
                        PEF::kPEFGlobalShare : PEF::kPEFProcessShare;
    write8(buf + 25, shareKind);                    // ShareKind
    write8(buf + 26, static_cast<uint8_t>(llvm::Log2_32(osec->getAlignment()))); // Alignment
    write8(buf + 27, 0);  // ReservedA (completes 28-byte header)

    buf += PEF::kSectionHeaderFileSize;  // Advance by 28 bytes
  }

  // Write loader section header (28 bytes)
  write32be(buf + 0, -1);  // NameOffset
  write32be(buf + 4, -1);  // DefaultAddress (-1 = no default address for loader)
  // BUG FIX #5: Loader section is NOT instantiated in memory!
  // TotalLength and UnpackedLength must be 0.
  // Only ContainerLength contains the actual size in the file.
  write32be(buf + 8, 0);   // TotalLength (0 = not instantiated)
  write32be(buf + 12, 0);  // UnpackedLength (0 = not unpacked)
  write32be(buf + 16, loaderData.size());  // ContainerLength
  write32be(buf + 20, loaderSectionOffset);  // ContainerOffset
  write8(buf + 24, PEF::kPEFLoaderSection); // SectionKind
  write8(buf + 25, PEF::kPEFGlobalShare);   // ShareKind
  write8(buf + 26, 4);  // Alignment (16 bytes = 2^4)
  write8(buf + 27, 0);  // ReservedA (completes 28-byte header)
}

void Writer::writeSections() {
  for (size_t i = 0; i < outputSections.size(); ++i) {
    OutputSection *osec = outputSections[i];
    // BUG FIX #13: Don't skip section if it contains TVect, even if no input sections
    bool hasTVect = (tvectSectionIndex >= 0 && static_cast<int16_t>(i) == tvectSectionIndex && !tvectData.empty());
    if (osec->getInputSections().empty() && !hasTVect)
      continue;

    uint8_t *buf = bufferStart + osec->getFileOffset();

    if (config->verbose) {
      const char *kindName = "unknown";
      if (osec->getKind() == PEF::kPEFCodeSection) kindName = "code";
      else if (isDataSection(osec->getKind())) kindName = "data";
      errorHandler().outs() << "Writing section " << i << " (" << kindName
                           << ") data at file offset 0x" << utohexstr(osec->getFileOffset()) << "\n";
    }

    // Data section with pattern-init encoding (CodeWarrior model)
    if (isDataSection(osec->getKind()) && osec->hasEncodedData()) {
      // Use already-encoded data from assignFileOffsets()
      const std::vector<uint8_t> &encoded = osec->getEncodedData();

      if (config->verbose) {
        errorHandler().outs() << "Pattern-encoded data section:\n"
                             << "  Unpacked size: " << osec->getUnpackedLength() << " bytes\n"
                             << "  Encoded size: " << encoded.size() << " bytes\n"
                             << "  Savings: " << (osec->getUnpackedLength() - encoded.size()) << " bytes\n";

        errorHandler().outs() << "  Encoded bytecode (hex): ";
        for (uint8_t byte : encoded) {
          errorHandler().outs() << format("%02x ", byte);
        }
        errorHandler().outs() << "\n";
      }

      // Write encoded pattern data to file
      memcpy(buf, encoded.data(), encoded.size());
      buf += encoded.size();

      if (config->verbose) {
        errorHandler().outs() << "Wrote pattern-encoded data section: " << encoded.size()
                             << " bytes at file offset 0x" << utohexstr(osec->getFileOffset()) << "\n";
      }
    }

    // Write each input section's data
    for (InputSection *isec : osec->getInputSections()) {
      if (config->verbose) {
        errorHandler().outs() << "Writing input section " << isec->getName()
                             << " (" << isec->getSize() << " bytes)"
                             << " at buffer offset " << (buf - bufferStart) << "\n";
      }

      // Check for patched code (3 sources in priority order):
      // 1. InputSection's patched data (from processRelocations)
      // 2. Writer's patchedCode map (from import stub replacement)
      // 3. Original data from input file

      if (isec->hasPatchedData()) {
        // Use patched data from relocation processing
        ArrayRef<uint8_t> data = isec->getPatchedData();
        memcpy(buf, data.data(), data.size());
        buf += data.size();
      } else {
        auto patchedIt = patchedCode.find(isec);
        if (patchedIt != patchedCode.end()) {
          // Use patched code from import stub replacement
          const std::vector<uint8_t> &data = patchedIt->second;
          memcpy(buf, data.data(), data.size());
          buf += data.size();
        } else {
          // Use original code
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

    // THEN write import stubs at the end (buf is now correctly positioned)
    if (osec->getKind() == PEF::kPEFCodeSection && !importStubs.empty()) {
      if (config->verbose) {
        errorHandler().outs() << "DEBUG: About to write stubs:\n"
                             << "  buf position: " << (buf - bufferStart) << "\n"
                             << "  section file offset: " << osec->getFileOffset() << "\n"
                             << "  stub buffer size: " << importStubs.size() << "\n"
                             << "  stub buffer.data() address: " << (void*)importStubs.data() << "\n"
                             << "  printing first " << std::min((size_t)24, importStubs.size()) << " bytes:\n    ";
        for (size_t i = 0; i < std::min((size_t)24, importStubs.size()); i++) {
          errorHandler().outs() << utohexstr(importStubs[i]) << " ";
          if (i % 8 == 7) errorHandler().outs() << "\n    ";
        }
        errorHandler().outs() << "\n  Total bytes in buffer: " << importStubs.size() << "\n";
      }

      memcpy(buf, importStubs.data(), importStubs.size());

      if (config->verbose) {
        errorHandler().outs() << "Wrote import stubs: " << importStubs.size()
                             << " bytes at offset 0x"
                             << utohexstr(buf - bufferStart - osec->getFileOffset())
                             << " in code section\n";
      }

      buf += importStubs.size();
    }

    // BUG FIX #23: TVect is now written inline in the data section above (lines 625-635)
    // No need to write it separately at the end
  }
}

void Writer::createEntryPointTVect() {
  // Find entry point symbol
  Symbol *entryPoint = nullptr;
  if (!config->entry.empty()) {
    entryPoint = symtab->find(config->entry);
  }

  if (!entryPoint || !entryPoint->isDefined()) {
    // No entry point - nothing to do
    tvectSectionIndex = -1;
    return;
  }

  auto *def = cast<Defined>(entryPoint);
  uint32_t entryOffset = def->getValue();

  // Find the data section (section 1 is typically .data)
  OutputSection *dataSection = nullptr;
  int16_t dataSectionIndex = -1;

  for (size_t i = 0; i < outputSections.size(); ++i) {
    if (isDataSection(outputSections[i]->getKind())) {
      dataSection = outputSections[i];
      dataSectionIndex = static_cast<int16_t>(i);
      break;
    }
  }

  if (!dataSection) {
    error("cannot create entry point TVect: no data section found");
    return;
  }

  // BUG FIX #21: TVect structure is 12 bytes: [code_address, toc_address, environment]
  // BUG FIX #10: TOC address calculation
  // The TOC address will be updated later after collectImports() calculates tocEntriesOffset
  // For now, use a placeholder (will be overwritten)
  uint32_t codeAddress = entryOffset;  // Offset within code section
  uint32_t tocAddress = 0;  // Placeholder - will be updated in updateEntryPointTVect()
  uint32_t environment = 0;  // Always 0 for executables

  // Create TVect data (12 bytes, big-endian)
  tvectData.resize(12);
  write32be(tvectData.data() + 0, codeAddress);
  write32be(tvectData.data() + 4, tocAddress);
  write32be(tvectData.data() + 8, environment);

  // TVect will be appended to data section
  tvectOffset = dataSection->getSize();
  tvectSectionIndex = dataSectionIndex;

  // Debug: verify TVect bytes
  if (config->verbose) {
    errorHandler().outs() << "TVect bytes: ";
    for (size_t i = 0; i < tvectData.size(); ++i) {
      errorHandler().outs() << format("%02x ", tvectData[i]);
    }
    errorHandler().outs() << "\n";
  }

  // Update data section size to include TVect
  // Note: We'll write the actual bytes in writeSections()

  if (config->verbose) {
    errorHandler().outs() << "Created entry point TVect:\n"
                         << "  Section: " << dataSectionIndex
                         << " Offset: 0x" << utohexstr(tvectOffset) << "\n"
                         << "  Code address: 0x" << utohexstr(codeAddress) << "\n"
                         << "  TOC address: 0x" << utohexstr(tocAddress) << "\n";
  }
}

// Update TVect TOC address for CodeWarrior model (direct import access)
void Writer::updateEntryPointTVect() {
  if (tvectSectionIndex < 0 || tvectData.empty()) {
    // No TVect created
    return;
  }

  // CodeWarrior model: r2 points to START of data section for direct import access
  // No TOC entries exist; import stubs load directly from import table
  // TVect.toc = 0 means: r2 = data_section_base + 0
  // Import stub "lwz r12, X(r2)" where X = import_index * 4
  uint32_t tocAddress = 0;

  // Update the TOC address in the TVect (second word, offset 4)
  write32be(tvectData.data() + 4, tocAddress);

  // TVect position stays after import table
  uint32_t importTableSize = totalImportedSymbolCount * 4;
  tvectOffset = importTableSize;  // TVect comes right after import table

  if (config->verbose) {
    errorHandler().outs() << "Updated entry point TVect (CodeWarrior model):\n"
                         << "  TOC address: 0x" << utohexstr(tocAddress)
                         << " (r2 = data section start)\n"
                         << "  TVect offset: 0x" << utohexstr(tvectOffset)
                         << " (after " << importTableSize << " byte import table)\n";
  }
}

// BUG FIX #35: Generate CodeWarrior-style import stubs in code section (24 bytes each)
void Writer::generateImportStubs() {
  if (importedLibraries.empty()) {
    return;
  }

  if (config->verbose) {
    errorHandler().outs() << "\nGenerating import stubs in code section...\n";
  }

  // Find data section to calculate TOC offsets
  OutputSection *dataSection = nullptr;
  for (auto *osec : outputSections) {
    if (isDataSection(osec->getKind())) {
      dataSection = osec;
      break;
    }
  }

  if (!dataSection) {
    error("cannot generate import stubs: no data section found");
    return;
  }

  // CodeWarrior model: r2 points to data section start (TVect.toc = 0)
  // Import stubs load DIRECTLY from import table using offset = index * 4
  // No TOC entries exist - this is the key architectural difference
  uint32_t tocBase = 0;  // r2 = data_section_base + 0

  // Generate one stub per imported symbol
  uint32_t stubIndex = 0;
  for (const auto &lib : importedLibraries) {
    for (ImportedSymbol *sym : lib.symbols) {
      uint32_t stubOffset = importStubs.size();
      stubOffsets[sym] = stubOffset;

      // Direct import table access: import slot at index * 4
      // For import 0: offset 0, for import 1: offset 4, etc.
      uint32_t importSlotOffset = stubIndex * 4;
      int32_t offsetFromTOC = importSlotOffset - tocBase;  // = stubIndex * 4

      // CodeWarrior-style import stub (24 bytes, 6 instructions)
      // Direct import table access - r2 points to data section start
      // r12 loaded from import table slot, which points to TVect in shared library
      //
      // Pattern (matches CodeWarrior exactly):
      // 1.  lwz r12, offset(r2)    # Load import table slot (offset = stubIndex * 4)
      // 2.  stw r2, 20(r1)         # Save our TOC on stack
      // 3.  lwz r0, 0(r12)         # Get function address from TVect[0]
      // 4.  lwz r2, 4(r12)         # Load imported function's TOC from TVect[1]
      // 5.  mtctr r0               # Set up call target
      // 6.  bctr                   # Branch to function (no return)

      // 1. lwz r12, offset(r2)  [Load import table slot directly]
      uint32_t lwz_r12 = 0x81820000 | (offsetFromTOC & 0xFFFF);
      importStubs.push_back((lwz_r12 >> 24) & 0xFF);
      importStubs.push_back((lwz_r12 >> 16) & 0xFF);
      importStubs.push_back((lwz_r12 >> 8) & 0xFF);
      importStubs.push_back(lwz_r12 & 0xFF);

      // 2. stw r2, 20(r1)  [Save current TOC to stack]
      uint32_t stw_r2 = 0x90410014;
      importStubs.push_back((stw_r2 >> 24) & 0xFF);
      importStubs.push_back((stw_r2 >> 16) & 0xFF);
      importStubs.push_back((stw_r2 >> 8) & 0xFF);
      importStubs.push_back(stw_r2 & 0xFF);

      // 3. lwz r0, 0(r12)  [Load function address from TVect[0]]
      uint32_t lwz_r0 = 0x800C0000;
      importStubs.push_back((lwz_r0 >> 24) & 0xFF);
      importStubs.push_back((lwz_r0 >> 16) & 0xFF);
      importStubs.push_back((lwz_r0 >> 8) & 0xFF);
      importStubs.push_back(lwz_r0 & 0xFF);

      // 4. lwz r2, 4(r12)  [Load imported function's TOC from TVect[1]]
      uint32_t lwz_r2_new = 0x804C0004;
      importStubs.push_back((lwz_r2_new >> 24) & 0xFF);
      importStubs.push_back((lwz_r2_new >> 16) & 0xFF);
      importStubs.push_back((lwz_r2_new >> 8) & 0xFF);
      importStubs.push_back(lwz_r2_new & 0xFF);

      // 5. mtctr r0  [Move function address to count register]
      uint32_t mtctr = 0x7C0903A6;
      importStubs.push_back((mtctr >> 24) & 0xFF);
      importStubs.push_back((mtctr >> 16) & 0xFF);
      importStubs.push_back((mtctr >> 8) & 0xFF);
      importStubs.push_back(mtctr & 0xFF);

      // 6. bctr  [Branch to function - no link, function never returns]
      // BUG FIX #35: Use bctr (0x4E800420) not bctrl (0x4E800421) for noreturn functions
      uint32_t bctr = 0x4E800420;
      importStubs.push_back((bctr >> 24) & 0xFF);
      importStubs.push_back((bctr >> 16) & 0xFF);
      importStubs.push_back((bctr >> 8) & 0xFF);
      importStubs.push_back(bctr & 0xFF);

      if (config->verbose) {
        errorHandler().outs() << "  Stub for " << sym->getName()
                             << " at offset 0x" << utohexstr(stubOffset)
                             << " (TOC offset: " << offsetFromTOC << ")\n";
      }

      stubIndex++;
    }
  }

  if (config->verbose) {
    errorHandler().outs() << "Generated " << stubOffsets.size()
                         << " import stubs (" << importStubs.size() << " bytes)\n";
  }
}

void Writer::generateTOCEntries() {
  if (importedSymbols.empty()) {
    return;
  }

  if (config->verbose) {
    errorHandler().outs() << "\nGenerating TOC entries in data section...\n";
  }

  // Find the data section
  OutputSection *dataSection = nullptr;
  for (OutputSection *osec : outputSections) {
    if (isDataSection(osec->getKind())) {
      dataSection = osec;
      break;
    }
  }

  if (!dataSection) {
    error("no data section found for TOC entries");
    return;
  }

  // TOC entries offset and size were already calculated in assignFileOffsets
  // Just verify they are set correctly
  if (config->verbose) {
    errorHandler().outs() << "  TOC entries at offset 0x" << utohexstr(tocEntriesOffset)
                         << " in data section\n";
    errorHandler().outs() << "  " << importedSymbols.size() << " entries × 12 bytes = "
                         << tocEntriesSize << " bytes\n";
  }
}


void Writer::replaceImportCalls() {
  if (config->verbose) {
    errorHandler().outs() << "\nReplacing bl .+1 with import stub calls...\n";
  }

  // Find the code section that will contain our stubs
  OutputSection *codeSection = nullptr;
  for (OutputSection *osec : outputSections) {
    if (osec->getKind() == PEF::kPEFCodeSection) {
      codeSection = osec;
      break;
    }
  }

  if (!codeSection) {
    error("no code section found");
    return;
  }

  // Scan all code sections for bl .+1 instructions that need replacement
  for (OutputSection *osec : outputSections) {
    if (osec->getKind() != PEF::kPEFCodeSection)
      continue;

    if (config->verbose) {
      errorHandler().outs() << "  Processing code section with "
                           << osec->getInputSections().size() << " input sections\n";
    }

    for (InputSection *isec : osec->getInputSections()) {
      // Get the code section data
      auto dataOrErr = isec->getData();
      if (!dataOrErr) {
        error("failed to get code section data: " + toString(dataOrErr.takeError()));
        continue;
      }

      // Make a mutable copy of the code
      std::vector<uint8_t> code(dataOrErr->begin(), dataOrErr->end());
      bool hasPatches = false;

      // Process relocations to find import calls
      ArrayRef<uint16_t> relocs = isec->getRelocations();
      uint32_t relocPos = 0;

      if (config->verbose) {
        errorHandler().outs() << "    Input section has " << relocs.size()
                             << " relocation instructions, code size " << code.size() << " bytes\n";
      }

      int importRelocCount = 0;
      for (size_t i = 0; i < relocs.size(); ) {
        uint16_t instr = endian::read16be(&relocs[i]);
        uint8_t opcode = (instr >> 9) & 0x7F;
        uint16_t operand = instr & 0x1FF;

        if (opcode == PEF::kPEFRelocSetPosition) {
          // Update position
          if (i + 1 < relocs.size()) {
            uint16_t instr2 = endian::read16be(&relocs[i + 1]);
            relocPos = (operand << 16) | instr2;
            i += 2;
          } else {
            i++;
          }
          continue;
        }

        if (opcode == PEF::kPEFRelocSmByImport || opcode == PEF::kPEFRelocLgByImport) {
          importRelocCount++;
          // Import relocation - get the import index
          uint32_t localIndex = operand;
          if (opcode == PEF::kPEFRelocLgByImport && i + 1 < relocs.size()) {
            uint16_t instr2 = endian::read16be(&relocs[i + 1]);
            localIndex = (operand << 16) | instr2;
            i++;
          }
          i++;

          // Look up the symbol
          ObjFile *objFile = dyn_cast<ObjFile>(isec->getFile());
          if (!objFile) {
            if (config->verbose) {
              errorHandler().outs() << "      WARNING: Not an ObjFile at import " << importRelocCount << "\n";
            }
            relocPos += 4;
            continue;
          }

          // The importIndexMap has Undefined symbols, but we need ImportedSymbol
          // Look up by name instead
          Symbol *undefinedSym = objFile->getImportSymbol(localIndex);
          if (!undefinedSym) {
            if (config->verbose) {
              errorHandler().outs() << "      WARNING: No symbol for local index " << localIndex << "\n";
            }
            relocPos += 4;
            continue;
          }

          // Find the corresponding ImportedSymbol by name
          Symbol *sym = symtab->find(undefinedSym->getName());
          if (!sym || !isa<ImportedSymbol>(sym)) {
            if (config->verbose) {
              errorHandler().outs() << "      WARNING: Symbol " << undefinedSym->getName()
                                   << " is not an imported symbol\n";
            }
            relocPos += 4;
            continue;
          }

          // Find the stub offset for this symbol
          auto it = stubOffsets.find(sym);
          if (it == stubOffsets.end()) {
            error("no stub found for imported symbol " + sym->getName());
            relocPos += 4;
            continue;
          }

          uint32_t stubOffset = it->second;

          // Calculate stub virtual address (in code section after all input sections)
          uint32_t stubVA = codeSection->getVirtualAddress() + codeSection->getOriginalSize() + stubOffset;

          // Calculate call site virtual address
          uint32_t codeVA = osec->getVirtualAddress() + isec->getVirtualAddress() + relocPos;

          // Calculate branch offset (in bytes, signed)
          int32_t branchOffset = stubVA - codeVA;

          // Check if code at relocPos contains bl .+1 (0x48000001)
          if (relocPos + 3 < code.size()) {
            uint32_t instruction = (code[relocPos] << 24) |
                                  (code[relocPos + 1] << 16) |
                                  (code[relocPos + 2] << 8) |
                                   code[relocPos + 3];

            // Debug first few import relocations
            if (importRelocCount <= 3 && config->verbose) {
              errorHandler().outs() << "      Import " << importRelocCount
                                   << " at relocPos 0x" << utohexstr(relocPos)
                                   << ": instruction = 0x" << utohexstr(instruction)
                                   << " (looking for 0x48000001)\n";
            }

            if (instruction == 0x48000001) {  // bl .+1
              // Patch with bl <stub>
              // bl instruction: 0x48 | (offset & 0x03FFFFFC) | 0x1
              uint32_t blInstr = 0x48000001 | (branchOffset & 0x03FFFFFC);

              code[relocPos] = (blInstr >> 24) & 0xFF;
              code[relocPos + 1] = (blInstr >> 16) & 0xFF;
              code[relocPos + 2] = (blInstr >> 8) & 0xFF;
              code[relocPos + 3] = blInstr & 0xFF;

              hasPatches = true;

              if (config->verbose) {
                errorHandler().outs() << "  Replaced bl .+1 at offset 0x" << utohexstr(relocPos)
                                     << " with call to stub at 0x" << utohexstr(stubVA)
                                     << " (offset=" << branchOffset << ") for " << sym->getName() << "\n";
              }
            } else if (config->verbose) {
              errorHandler().outs() << "  WARNING: Expected bl .+1 at offset 0x" << utohexstr(relocPos)
                                   << " but found 0x" << utohexstr(instruction) << "\n";
            }
          }

          relocPos += 4;
        } else {
          // Other relocation types - skip
          if (opcode == PEF::kPEFRelocBySectC || opcode == PEF::kPEFRelocBySectD) {
            // Run length encoding - operand + 1 relocations
            relocPos += 4 * (operand + 1);
          } else {
            relocPos += 4;
          }
          i++;
        }
      }

      // Store patched code if we made changes
      if (hasPatches) {
        patchedCode[isec] = std::move(code);
        if (config->verbose) {
          errorHandler().outs() << "    Patched input section with import stub calls\n";
        }
      } else if (config->verbose) {
        errorHandler().outs() << "    No patches made for this input section (found "
                             << importRelocCount << " import relocations)\n";
      }
    }
  }

  if (config->verbose) {
    errorHandler().outs() << "  Total sections patched: " << patchedCode.size() << "\n";
  }
}

void Writer::writeLoaderSection() {
  // Use the loader section offset calculated in assignFileOffsets()
  uint8_t *buf = bufferStart + loaderSectionOffset;
  memcpy(buf, loaderData.data(), loaderData.size());
}

void Writer::run() {
  if (config->verbose) {
    errorHandler().outs() << "\nWriting PEF executable...\n";
  }

  // Create entry point transition vector (must be done before assignFileOffsets)
  createEntryPointTVect();

  // Collect imports BEFORE assigning file offsets (so section sizes are correct)
  collectImports();

  // Assign file offsets to sections (this calculates tocEntriesOffset)
  assignFileOffsets();

  // Assign virtual addresses to all defined symbols (after layout)
  assignSymbolAddresses();

  // BUG FIX #10 & #15: Update TVect TOC address and offset after assignFileOffsets
  updateEntryPointTVect();

  // Generate import stubs in code section
  generateImportStubs();

  // Generate TOC entries in data section
  generateTOCEntries();

  // Assign virtual addresses to imported symbols
  assignImportAddresses();

  // Replace bl .+1 instructions with calls to import stubs
  replaceImportCalls();

  // BUG FIX #15: Create loader section AFTER tvectOffset is finalized
  createLoaderSection();

  // Update file size with actual loader section size
  fileSize = loaderSectionOffset + loaderData.size();

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
