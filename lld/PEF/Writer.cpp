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
#include <set>
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
  void collectFunctions();        // Collect and sort all code functions
  void createFunctionTVectors();  // Create TVector offset map for functions
  void generateImportStubs();     // Generate stubs in code section
  void generateTOCEntries();      // Generate TOC entries in data section
  void replaceImportCalls();      // Replace bl .+1 with calls to import stubs
  void optimizeTOCRestores();     // Optimize TOC restores for same-fragment calls
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

  // Flag to indicate if this is the final assignFileOffsets call
  bool finalFileOffsetPass = false;
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

  // Function TVector table (for function pointers)
  // Maps each defined code symbol to its TVector offset in data section
  std::map<Symbol*, uint32_t> functionTVectors;  // Symbol -> offset in data section
  std::vector<Defined*> codeFunctions;  // Sorted list of code functions (for TVector writing order)
  uint32_t functionTVectorsOffset = 0;  // Start offset of TVector table in data section
  uint32_t functionTVectorsSize = 0;    // Total size of TVector table

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

  // BUG FIX: Loader section must come FIRST after headers (CodeWarrior convention)
  // This prevents code/data from overwriting the loader section header area
  offset = alignTo(offset, 16);
  loaderSectionOffset = offset;

  // Reserve space for loader section (will be created later in run())
  // Estimate size conservatively - actual size will be determined when created
  // CodeWarrior uses ~156 bytes for simple programs, allow 256 bytes for safety
  uint32_t estimatedLoaderSize = 256;
  offset += estimatedLoaderSize;

  if (config->verbose) {
    errorHandler().outs() << "Loader section offset: 0x" << utohexstr(loaderSectionOffset)
                         << ", estimated size: " << estimatedLoaderSize << " bytes\n";
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

    // BUG FIX: Use original size on subsequent calls to avoid double-counting additions
    // On first call, originalSize will be 0 (not set yet), so we use getSize()
    // On second call, originalSize will have the base size without additions
    uint64_t sectionSize = osec->getOriginalSize();
    if (sectionSize == 0) {
      sectionSize = osec->getSize();
      // Save the original size for subsequent calls
      osec->setOriginalSize(sectionSize);
    }

    // If this is the data section, reserve space for import table (CodeWarrior model)
    if (isDataSection(osec->getKind())) {
      // Reserve space for import address table (4 bytes per import)
      uint32_t importTableSize = totalImportedSymbolCount * 4;

      // CodeWarrior model: NO TOC entries, use direct import table access
      // r2 points to start of data section, stubs load directly from import table
      tocEntriesSize = 0;
      tocEntriesOffset = 0;

      // Data section layout: [Import table][Padding (8 bytes)][Entry Point TVector (8 bytes)][User data]
      // CodeWarrior model: Only ONE TVector for the entry point, no function TVector table
      // CodeWarrior adds 8 bytes of padding between import table and TVector
      // Using TVector8 format to match CodeWarrior
      uint32_t paddingSize = 8;  // CodeWarrior adds 8 bytes padding before TVector
      uint32_t entryPointTVectorSize = 8;  // Only the entry point TVector
      sectionSize += importTableSize + paddingSize + entryPointTVectorSize;

      if (config->verbose && (importTableSize > 0 || entryPointTVectorSize > 0)) {
        errorHandler().outs() << "Data section additions (CodeWarrior TVector8 model):\n"
                             << "  Import table: " << importTableSize << " bytes\n"
                             << "  Padding: " << paddingSize << " bytes\n"
                             << "  Entry point TVector8: " << entryPointTVectorSize << " bytes\n"
                             << "  Total: " << (importTableSize + paddingSize + entryPointTVectorSize) << " bytes\n"
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

      // Add 8 bytes of padding between import table and TVector (CodeWarrior model)
      uint32_t paddingSize = 8;
      dataContent.insert(dataContent.end(), paddingSize, 0);

      // CodeWarrior model: Only write ONE TVector for the entry point
      // Do NOT write TVectors for all functions - that causes data section bloat and out-of-bounds relocations

      // Find code section to get its base address for calculating section-relative offsets
      OutputSection *codeSection = nullptr;
      for (OutputSection *sec : outputSections) {
        if (sec->getKind() == PEF::kPEFCodeSection) {
          codeSection = sec;
          break;
        }
      }
      uint64_t codeBaseVA = codeSection ? codeSection->getVirtualAddress() : 0;

      // Find the entry point function (usually "__start")
      Defined *entryFunc = nullptr;
      for (Defined *funcDef : codeFunctions) {
        if (funcDef->getName() == config->entry) {
          entryFunc = funcDef;
          break;
        }
      }

      // Write ONLY the entry point TVector
      if (entryFunc) {
        // CRITICAL FIX: Use getVirtualAddress() which returns final linked address
        // getValue() returns 0 for unassigned symbols, causing TVectors to be all zeros
        // CFM will add section base address via TVector8 relocations at load time
        uint32_t codeAddress = entryFunc->getVirtualAddress() - codeBaseVA;  // Section-relative offset
        uint32_t tocAddress = 0;  // r2 points to data section start (CodeWarrior model)

        // Write TVector8 as 8 bytes (big-endian) - no environment field
        uint8_t tvectorBytes[8];
        write32be(tvectorBytes + 0, codeAddress);
        write32be(tvectorBytes + 4, tocAddress);

        dataContent.insert(dataContent.end(), tvectorBytes, tvectorBytes + 8);

        if (config->verbose) {
          errorHandler().outs() << "  Writing entry point TVector8 for " << entryFunc->getName()
                               << ": code=0x" << utohexstr(codeAddress)
                               << " (getValue()=" << utohexstr(entryFunc->getValue())
                               << " VA=" << utohexstr(entryFunc->getVirtualAddress())
                               << " finalPass=" << (finalFileOffsetPass ? "yes" : "no")
                               << ")\n";
        }
      }

      // BUG FIX: Include original input section data (e.g., global variables)
      // This was missing, causing global variables to not be included in the output!
      // CRITICAL: Use patched data if available (after relocations are processed)
      for (InputSection *isec : osec->getInputSections()) {
        ArrayRef<uint8_t> inputData;

        // Prefer patched data (with relocations applied) over raw data
        if (isec->hasPatchedData()) {
          inputData = isec->getPatchedData();
          if (config->verbose) {
            errorHandler().outs() << "  Including PATCHED input section data from "
                                 << isec->getFile()->getName() << ": "
                                 << inputData.size() << " bytes\n";
          }
        } else {
          auto dataOrErr = isec->getData();
          if (!dataOrErr) {
            error(isec->getFile()->getName() + ": failed to get data for section " +
                  Twine(isec->getIndex()));
            continue;
          }
          inputData = *dataOrErr;
          if (config->verbose) {
            errorHandler().outs() << "  Including input section data from "
                                 << isec->getFile()->getName() << ": "
                                 << inputData.size() << " bytes\n";
          }
        }

        dataContent.insert(dataContent.end(), inputData.begin(), inputData.end());
      }

      // Only encode data on the final pass when function addresses are known
      if (finalFileOffsetPass) {
        // DEBUG: Show what's in dataContent before encoding
        if (config->verbose) {
          errorHandler().outs() << "DataContent before encoding (" << dataContent.size() << " bytes):\n";
          for (size_t i = 0; i < std::min<size_t>(64, dataContent.size()); i++) {
            if (i % 16 == 0) errorHandler().outs() << "  " << utohexstr(i, 4) << ": ";
            errorHandler().outs() << utohexstr(dataContent[i], 2) << " ";
            if (i % 16 == 15) errorHandler().outs() << "\n";
          }
          if (dataContent.size() > 64) errorHandler().outs() << "  ... (truncated)\n";
          if (dataContent.size() % 16 != 0) errorHandler().outs() << "\n";
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
      } else {
        // First pass: just use unencoded size as estimate
        osec->setUnpackedLength(dataContent.size());
        sizeInFile = dataContent.size();  // Estimate (will be corrected on final pass)
      }
    }

    // BUG FIX: Add 8 bytes of padding to code sections (CodeWarrior convention)
    // This prevents CFM validation errors and matches CodeWarrior's layout
    uint64_t alignedSize = sizeInFile;

    // For code sections, add exactly 8 bytes of padding
    if (osec->getKind() == PEF::kPEFCodeSection) {
      alignedSize = sizeInFile + 8;
      osec->setSize(alignedSize);
      sectionSize = alignedSize;
      sizeInFile = alignedSize;
    }

    if (config->verbose) {
      const char *kindName = "unknown";
      if (osec->getKind() == PEF::kPEFCodeSection) kindName = "code";
      else if (isDataSection(osec->getKind())) kindName = "data";
      errorHandler().outs() << "Section " << i << " (" << kindName
                           << "): fileOffset=0x" << utohexstr(osec->getFileOffset())
                           << ", size=" << sectionSize
                           << ", file size=" << sizeInFile;
      if (alignedSize != (osec->getKind() == PEF::kPEFCodeSection ? sizeInFile : alignTo(sizeInFile, 16))) {
        errorHandler().outs() << " (+" << (alignedSize - sizeInFile) << " padding)";
      }
      errorHandler().outs() << "\n";
    }

    offset += alignedSize;
  }

  // BUG FIX: Loader section offset was already set at the beginning (before code/data)
  // Update file size to include everything written so far
  fileSize = offset;
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

    // BUG FIX: For data sections, account for import table and function TVectors that come before original data
    uint64_t dataOffset = 0;
    if (isDataSection(osec->getKind())) {
      uint32_t importTableSize = totalImportedSymbolCount * 4;
      dataOffset = importTableSize + functionTVectorsSize;  // Import table + Function TVectors (no separate main TVect)
    }

    // Get all defined symbols from the symbol table
    std::vector<Defined*> allDefined = symtab->getDefinedSymbols();

    for (Defined *sym : allDefined) {
      // Only process symbols in this section
      if (sym->getSectionIndex() != static_cast<int16_t>(secIdx))
        continue;

      // Calculate virtual address: section base + data section offset + symbol offset
      uint64_t virtualAddr = sectionBase + dataOffset + sym->getValue();
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
  // IMPORTANT: Preserve insertion order from symbol table - import remapping depends on this!
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
  // Clear any previous loader data
  loaderData.clear();

  // Phase 3: Generate relocation instructions
  // Note: collectImports() is now called earlier in run() before assignFileOffsets()
  uint32_t numFunctionTVectors = functionTVectorsSize / 8;  // Each TVector8 is 8 bytes
  PEFRelocWriter relocWriter(outputSections, importedLibraries, numFunctionTVectors);
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

  // BUG FIX: MainSection/MainOffset MUST point to entry point's TVector in function table
  // NO separate main TVect - use __start's function TVector from the table
  // Find __start in the function TVector table
  Defined *entryFunc = nullptr;
  uint32_t entryTVectorOffset = 0;
  int16_t entryTVectorSection = -1;

  // Calculate import table size for mainOffset calculation
  uint32_t importTableSize = totalImportedSymbolCount * 4;

  for (const auto &entry : functionTVectors) {
    Defined *func = cast<Defined>(entry.first);
    if (func->getName() == config->entry) {
      entryFunc = func;
      // CRITICAL FIX: Entry TVector is at importTableSize + padding offset
      // CodeWarrior adds 8 bytes of padding between import table and TVector
      // Don't use entry.second from map - that assumes multiple TVectors exist
      uint32_t paddingSize = 8;  // CodeWarrior adds 8 bytes padding before TVector
      entryTVectorOffset = importTableSize + paddingSize;
      // Function TVectors are in the data section
      for (size_t i = 0; i < outputSections.size(); ++i) {
        if (isDataSection(outputSections[i]->getKind())) {
          entryTVectorSection = i;
          break;
        }
      }
      break;
    }
  }

  if (entryFunc && entryTVectorSection >= 0) {
    // Entry point is the function's TVector in the function table
    if (config->verbose) {
      errorHandler().outs() << "Entry point TVector: " << config->entry
                           << " MainSection=" << entryTVectorSection
                           << " MainOffset=0x" << utohexstr(entryTVectorOffset) << "\n";
    }

    write32be(ptr + 0, entryTVectorSection);  // MainSection (data section)
    write32be(ptr + 4, entryTVectorOffset);   // MainOffset (offset of entry function's TVector)
  } else {
    // Fallback: no entry point found
    error("entry point function '" + config->entry + "' not found in function TVector table");
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
      // BUG FIX: TotalLength should match UnpackedLength for pattern data
      // getSize() may include padding or temporary calculations
      uint32_t unpackedLength = osec->getUnpackedLength();  // Size of pattern data
      uint32_t totalLength = unpackedLength;  // Total size in memory (same as unpacked)
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
    // BUG FIX: Code sections use Global sharing, Data sections use Process sharing
    // This matches CodeWarrior's convention
    uint8_t shareKind = (osec->getKind() == PEF::kPEFCodeSection) ?
                        PEF::kPEFGlobalShare : PEF::kPEFProcessShare;
    write8(buf + 25, shareKind);                    // ShareKind
    write8(buf + 26, static_cast<uint8_t>(llvm::Log2_32(osec->getAlignment()))); // Alignment
    write8(buf + 27, 0);  // ReservedA (completes 28-byte header)

    buf += PEF::kSectionHeaderFileSize;  // Advance by 28 bytes
  }

  // Write loader section header (28 bytes)
  write32be(buf + 0, -1);  // NameOffset
  write32be(buf + 4, 0);  // DefaultAddress (0 = loader not loaded into memory)
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

        // DEBUG: Show critical TVector data
        errorHandler().outs() << "  Data section layout:\n"
                             << "    Import table: 0x0 - 0x" << utohexstr(totalImportedSymbolCount * 4) << "\n"
                             << "    Function TVectors: 0x" << utohexstr(functionTVectorsOffset)
                             << " - 0x" << utohexstr(functionTVectorsOffset + functionTVectorsSize) << "\n";

        // Show entry point TVector location
        for (const auto &entry : functionTVectors) {
          Defined *func = cast<Defined>(entry.first);
          if (func->getName() == config->entry) {
            errorHandler().outs() << "    Entry point TVector (" << func->getName()
                                 << "): offset 0x" << utohexstr(entry.second)
                                 << " (code VA 0x" << utohexstr(func->getVirtualAddress())
                                 << ", should match mainOffset from loader)\n";
            break;
          }
        }
      }

      // Write encoded pattern data to file
      memcpy(buf, encoded.data(), encoded.size());
      buf += encoded.size();

      if (config->verbose) {
        errorHandler().outs() << "Wrote pattern-encoded data section: " << encoded.size()
                             << " bytes at file offset 0x" << utohexstr(osec->getFileOffset()) << "\n";
      }

      // BUG FIX: Skip writing input sections - they're already included in pattern data
      // The pattern-encoded data was built from input sections in assignFileOffsets()
      continue;  // Skip to next output section
    }

    // Write each input section's data (only for non-pattern sections like code)
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

    // BUG FIX: Write padding bytes to align section to 16-byte boundary
    // Calculate how much padding is needed based on what was written
    uint64_t written = buf - (bufferStart + osec->getFileOffset());
    uint64_t aligned = alignTo(written, 16);
    uint64_t padding = aligned - written;

    if (padding > 0) {
      memset(buf, 0, padding);
      if (config->verbose) {
        errorHandler().outs() << "Wrote " << padding << " padding bytes to align section\n";
      }
      buf += padding;
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

  // Create TVector8 data (8 bytes, big-endian) - matching CodeWarrior
  tvectData.resize(8);
  write32be(tvectData.data() + 0, codeAddress);
  write32be(tvectData.data() + 4, tocAddress);

  // TVect will be appended to data section
  tvectOffset = dataSection->getSize();
  tvectSectionIndex = dataSectionIndex;

  // Debug: verify TVect bytes
  if (config->verbose) {
    errorHandler().outs() << "TVector8 bytes: ";
    for (size_t i = 0; i < tvectData.size(); ++i) {
      errorHandler().outs() << format("%02x ", tvectData[i]);
    }
    errorHandler().outs() << "\n";
  }

  // Update data section size to include TVect
  // Note: We'll write the actual bytes in writeSections()

  if (config->verbose) {
    errorHandler().outs() << "Created entry point TVector8:\n"
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

// Create TVectors for all defined function symbols
// This allows function pointers to work correctly with CFM calling convention
void Writer::createFunctionTVectors() {
  if (config->verbose) {
    errorHandler().outs() << "\nCreating function TVectors for function pointers...\n";
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
    if (config->verbose) {
      errorHandler().outs() << "  No data section found, skipping TVector creation\n";
    }
    return;
  }

  // Collect all defined code symbols (functions) that are actually in code sections
  // Note: codeFunctions is now a member variable so it can be used in assignFileOffsets()
  codeFunctions.clear();  // Clear any previous data
  for (OutputSection *osec : outputSections) {
    if (osec->getKind() != PEF::kPEFCodeSection)
      continue;

    for (InputSection *isec : osec->getInputSections()) {
      ObjFile *file = isec->getFile();
      unsigned sectionIndex = isec->getIndex();

      for (Symbol *sym : file->getSymbols()) {
        if (!sym->isDefined())
          continue;

        auto *def = cast<Defined>(sym);
        // Only create TVectors for code symbols that are in THIS code section
        if (def->getSymbolClass() == PEF::kPEFCodeSymbol &&
            def->getSectionIndex() == static_cast<int16_t>(sectionIndex)) {
          codeFunctions.push_back(def);
        }
      }
    }
  }

  if (codeFunctions.empty()) {
    if (config->verbose) {
      errorHandler().outs() << "  No code functions found\n";
    }
    return;
  }

  // Sort functions by code address to match CodeWarrior's order
  // This ensures consistent TVector table layout
  // IMPORTANT: Use getVirtualAddress() not getValue() for multi-file linking!
  // getValue() returns input section offset, which can be the same (0) for
  // symbols from different .o files, leading to non-deterministic sort order

  if (config->verbose) {
    errorHandler().outs() << "  Before sort:\n";
    for (const Defined *func : codeFunctions) {
      errorHandler().outs() << "    " << func->getName()
                           << ": VA=0x" << utohexstr(func->getVirtualAddress())
                           << " value=0x" << utohexstr(func->getValue()) << "\n";
    }
  }

  std::sort(codeFunctions.begin(), codeFunctions.end(),
            [](const Defined *a, const Defined *b) {
              return a->getVirtualAddress() < b->getVirtualAddress();
            });

  if (config->verbose) {
    errorHandler().outs() << "  After sort - Found " << codeFunctions.size() << " code functions\n";
    for (const Defined *func : codeFunctions) {
      errorHandler().outs() << "    " << func->getName()
                           << ": VA=0x" << utohexstr(func->getVirtualAddress()) << "\n";
    }
  }

  // Calculate TVector table offset
  // CRITICAL FIX: CodeWarrior model uses only ONE TVector (entry point), not a table
  // Only ONE TVector is written for the entry point, regardless of how many functions exist
  uint32_t importTableSize = totalImportedSymbolCount * 4;
  functionTVectorsOffset = importTableSize;  // Immediately after import table
  functionTVectorsSize = 8;  // Only ONE TVector for entry point (not codeFunctions.size() * 8)

  // Create TVector8 for each function
  uint32_t currentOffset = functionTVectorsOffset;
  for (Defined *func : codeFunctions) {
    functionTVectors[func] = currentOffset;

    if (config->verbose) {
      errorHandler().outs() << "  " << func->getName()
                           << ": TVector8 at offset 0x" << utohexstr(currentOffset)
                           << " (code at 0x" << utohexstr(func->getValue()) << ")\n";
    }

    currentOffset += 8;
  }

  if (config->verbose) {
    errorHandler().outs() << "  TVector table: offset=0x" << utohexstr(functionTVectorsOffset)
                         << " size=" << functionTVectorsSize << " bytes\n";
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

// Helper function to patch lis/addi instruction pairs for data references
// PowerPC uses lis (load immediate shifted) + addi to load 32-bit addresses
// Example: lis r3, hi; addi r3, r3, lo  loads address into r3
static bool patchLisAddiPair(std::vector<uint8_t> &code, uint32_t offset,
                             uint64_t targetAddr, StringRef symName) {
  // Verify we have space for both instructions (8 bytes total)
  if (offset + 7 >= code.size()) {
    error("lis/addi pair at offset " + Twine(offset) + " extends beyond code section");
    return false;
  }

  // Read both instructions (big-endian)
  uint32_t lisInstr = (code[offset + 0] << 24) | (code[offset + 1] << 16) |
                      (code[offset + 2] << 8) | code[offset + 3];
  uint32_t addiInstr = (code[offset + 4] << 24) | (code[offset + 5] << 16) |
                       (code[offset + 6] << 8) | code[offset + 7];

  // Verify lis instruction (opcode 15, format: lis rD, imm)
  uint8_t lisOpcode = (lisInstr >> 26) & 0x3F;
  if (lisOpcode != 15) {
    if (config->verbose) {
      errorHandler().outs() << "  WARNING: Expected lis instruction at offset 0x"
                           << utohexstr(offset) << " but found opcode "
                           << (unsigned)lisOpcode << "\n";
    }
    return false;
  }

  // Verify second instruction (opcode 14=addi or 36=stw)
  uint8_t secondOpcode = (addiInstr >> 26) & 0x3F;
  if (secondOpcode != 14 && secondOpcode != 36) {
    if (config->verbose) {
      errorHandler().outs() << "  WARNING: Expected addi/stw instruction at offset 0x"
                           << utohexstr(offset + 4) << " but found opcode "
                           << (unsigned)secondOpcode << "\n";
    }
    return false;
  }

  // Extract register fields
  // lis: rD = bits 25-21, rA = bits 20-16 (should be 0)
  // addi/stw: rD/rS = bits 25-21, rA = bits 20-16 (should match lis rD)
  uint8_t lisRD = (lisInstr >> 21) & 0x1F;
  uint8_t secondRD = (addiInstr >> 21) & 0x1F;  // For stw, this is rS (source)
  uint8_t secondRA = (addiInstr >> 16) & 0x1F;  // Base register

  // Validate that second instruction uses result from lis
  // For addi: rA must match lis rD
  // For stw: rA must match lis rD (using lis result as base address)
  if (secondRA != lisRD) {
    if (config->verbose) {
      const char *instrName = (secondOpcode == 14) ? "addi" : "stw";
      errorHandler().outs() << "  WARNING: Register mismatch in lis/" << instrName << " pair at 0x"
                           << utohexstr(offset) << " (lis r" << (unsigned)lisRD
                           << ", " << instrName << " r" << (unsigned)secondRD << ", r"
                           << (unsigned)secondRA << ")\n";
    }
    return false;
  }

  // Extract the current offset from the instructions
  // The compiler emits the offset within the section
  uint16_t currentHigh = lisInstr & 0xFFFF;
  int16_t currentLow = (int16_t)(addiInstr & 0xFFFF);  // Sign-extended

  // Reconstruct the current offset (handle sign extension)
  uint32_t currentOffset = (currentHigh << 16) + currentLow;

  // Add section base address to offset to get final address
  uint64_t finalAddr = targetAddr + currentOffset;

  // Split final address into high and low 16 bits
  // PowerPC sign-extends the low half, so adjust high half if needed
  uint16_t low = finalAddr & 0xFFFF;
  uint16_t high = (finalAddr >> 16) & 0xFFFF;

  // If low half is negative (bit 15 set), increment high half
  // This compensates for sign extension in addi
  if (low & 0x8000) {
    high += 1;
  }

  // Patch lis instruction: keep opcode and registers, replace immediate
  uint32_t newLisInstr = (lisInstr & 0xFFFF0000) | high;
  code[offset + 0] = (newLisInstr >> 24) & 0xFF;
  code[offset + 1] = (newLisInstr >> 16) & 0xFF;
  code[offset + 2] = (newLisInstr >> 8) & 0xFF;
  code[offset + 3] = newLisInstr & 0xFF;

  // Patch addi instruction: keep opcode and registers, replace immediate
  uint32_t newAddiInstr = (addiInstr & 0xFFFF0000) | low;
  code[offset + 4] = (newAddiInstr >> 24) & 0xFF;
  code[offset + 5] = (newAddiInstr >> 16) & 0xFF;
  code[offset + 6] = (newAddiInstr >> 8) & 0xFF;
  code[offset + 7] = newAddiInstr & 0xFF;

  if (config->verbose) {
    errorHandler().outs() << "  Patched lis/addi pair at offset 0x" << utohexstr(offset)
                         << " for '" << symName << "' (base=0x" << utohexstr(targetAddr)
                         << " + offset=0x" << utohexstr(currentOffset) << " = 0x"
                         << utohexstr(finalAddr) << ", high=0x" << utohexstr(high)
                         << ", low=0x" << utohexstr(low) << ")\n";
  }

  return true;
}

void Writer::replaceImportCalls() {
  if (config->verbose) {
    errorHandler().outs() << "\nReplacing bl .+1 and lis/addi pairs with import stub calls...\n";
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

      // Track positions we've already patched (for lis/addi pairs)
      std::set<uint32_t> patchedPositions;

      if (config->verbose) {
        errorHandler().outs() << "    Input section has " << relocs.size()
                             << " relocation instructions, code size " << code.size() << " bytes\n";
      }

      int importRelocCount = 0;
      for (size_t i = 0; i < relocs.size(); ) {
        uint16_t instr = endian::read16be(&relocs[i]);
        uint8_t opcode = (instr >> 9) & 0x7F;
        uint16_t operand = instr & 0x1FF;

        if (config->verbose && (opcode == PEF::kPEFRelocBySectC || opcode == PEF::kPEFRelocBySectD)) {
          errorHandler().outs() << "    DEBUG READ: i=" << i << " instr=0x" << utohexstr(instr)
                               << " opcode=0x" << utohexstr(opcode)
                               << " operand=" << operand
                               << " type=" << (opcode == PEF::kPEFRelocBySectC ? "BySectC" : "BySectD")
                               << "\n";
        }

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

          // Find the corresponding symbol by name
          Symbol *sym = symtab->find(undefinedSym->getName());
          if (!sym) {
            if (config->verbose) {
              errorHandler().outs() << "      WARNING: Symbol " << undefinedSym->getName()
                                   << " not found in symbol table\n";
            }
            relocPos += 4;
            continue;
          }

          // Calculate call site virtual address
          uint32_t codeVA = osec->getVirtualAddress() + isec->getVirtualAddress() + relocPos;

          // Calculate branch offset based on symbol type
          int32_t branchOffset;

          if (auto *defined = dyn_cast<Defined>(sym)) {
            // Internal symbol - branch directly to the defined symbol
            uint64_t targetVA = defined->getVirtualAddress();
            branchOffset = static_cast<int32_t>(targetVA - codeVA);

            if (config->verbose) {
              errorHandler().outs() << "      Internal call to '" << sym->getName()
                                   << "' at 0x" << utohexstr(relocPos)
                                   << " targeting 0x" << utohexstr(targetVA) << "\n";
            }
          } else if (isa<ImportedSymbol>(sym)) {
            // External imported symbol - branch to stub
            auto it = stubOffsets.find(sym);
            if (it == stubOffsets.end()) {
              error("no stub found for imported symbol " + sym->getName());
              relocPos += 4;
              continue;
            }

            uint32_t stubOffset = it->second;
            uint32_t stubVA = codeSection->getVirtualAddress() + codeSection->getOriginalSize() + stubOffset;
            branchOffset = stubVA - codeVA;
          } else {
            if (config->verbose) {
              errorHandler().outs() << "      WARNING: Symbol " << undefinedSym->getName()
                                   << " has unexpected type\n";
            }
            relocPos += 4;
            continue;
          }

          // Skip if we've already patched this position (e.g., second half of lis/addi pair)
          if (patchedPositions.count(relocPos)) {
            if (config->verbose) {
              errorHandler().outs() << "      Import " << importRelocCount
                                   << " at relocPos 0x" << utohexstr(relocPos)
                                   << " already patched, skipping\n";
            }
            relocPos += 4;
            continue;
          }

          // Track the original relocPos before any adjustment
          uint32_t originalRelocPos = relocPos;

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
                                   << ": instruction = 0x" << utohexstr(instruction) << "\n";
            }

            // Decode instruction opcode to determine type
            uint8_t instrOpcode = (instruction >> 26) & 0x3F;

            // SPECIAL CASE: CodeWarrior points lis/addi relocations to immediate field (+2 bytes)
            // If we read opcode 0, check if we're at +2 offset into a lis instruction
            if (instrOpcode == 0 && relocPos >= 2) {
              // Try reading 2 bytes earlier to check for lis
              uint32_t prevInstr = (code[relocPos - 2] << 24) |
                                   (code[relocPos - 1] << 16) |
                                   (code[relocPos] << 8) |
                                    code[relocPos + 1];
              uint8_t prevOpcode = (prevInstr >> 26) & 0x3F;

              if (prevOpcode == 15) {  // Found lis instruction
                // Adjust relocPos to instruction start
                relocPos -= 2;
                instruction = prevInstr;
                instrOpcode = 15;

                if (config->verbose) {
                  errorHandler().outs() << "      Adjusted relocPos -2 bytes (lis immediate field → instruction start)\n";
                }
              }
            }

            if (instrOpcode == 18 && (instruction & 0x03FFFFFC) == 0) {
              // This is bl .+1 - branch instruction (function call)
              // Patch with bl <stub>
              // bl instruction: 0x48 | (offset & 0x03FFFFFC) | 0x1
              uint32_t blInstr = 0x48000001 | (branchOffset & 0x03FFFFFC);

              code[relocPos] = (blInstr >> 24) & 0xFF;
              code[relocPos + 1] = (blInstr >> 16) & 0xFF;
              code[relocPos + 2] = (blInstr >> 8) & 0xFF;
              code[relocPos + 3] = blInstr & 0xFF;

              hasPatches = true;

              if (config->verbose) {
                uint64_t targetAddr = codeVA + branchOffset;
                errorHandler().outs() << "  Replaced bl .+1 at offset 0x" << utohexstr(relocPos)
                                     << " with call to 0x" << utohexstr(targetAddr)
                                     << " (offset=" << branchOffset << ") for " << sym->getName() << "\n";
              }
            } else if (instrOpcode == 15) {
              // This is lis - data reference (taking address of function)
              // Pattern: lis rD, imm followed by addi rD, rD, imm
              // Calculate target address (stub for imports, symbol VA for internal)
              uint64_t targetAddr;
              if (isa<ImportedSymbol>(sym)) {
                // For imported symbols, use stub address
                auto it = stubOffsets.find(sym);
                if (it == stubOffsets.end()) {
                  error("no stub found for imported symbol " + sym->getName());
                  relocPos += 4;
                  continue;
                }
                uint32_t stubOffset = it->second;
                targetAddr = codeSection->getVirtualAddress() +
                            codeSection->getOriginalSize() + stubOffset;
              } else if (auto *defined = dyn_cast<Defined>(sym)) {
                // For defined symbols, check if it's a code symbol (function)
                // If so, use TVector address instead of code address
                if (defined->getSymbolClass() == PEF::kPEFCodeSymbol) {
                  // This is a function - use its TVector address
                  auto tvIt = functionTVectors.find(defined);
                  if (tvIt != functionTVectors.end()) {
                    // Found TVector - use data section address + TVector offset
                    OutputSection *dataSection = nullptr;
                    for (OutputSection *osec : outputSections) {
                      if (isDataSection(osec->getKind())) {
                        dataSection = osec;
                        break;
                      }
                    }
                    if (dataSection) {
                      targetAddr = dataSection->getVirtualAddress() + tvIt->second;
                      if (config->verbose) {
                        errorHandler().outs() << "      Redirecting function pointer to TVector at offset "
                                             << tvIt->second << " (VA=0x" << utohexstr(targetAddr) << ")\n";
                      }
                    } else {
                      error("no data section found for TVector");
                      relocPos += 4;
                      continue;
                    }
                  } else {
                    // No TVector found - this shouldn't happen
                    warn("function " + sym->getName() + " has no TVector entry");
                    targetAddr = defined->getVirtualAddress();
                  }
                } else {
                  // Data symbol - use its virtual address directly
                  targetAddr = defined->getVirtualAddress();
                }
              } else {
                if (config->verbose) {
                  errorHandler().outs() << "  WARNING: Cannot patch lis/addi for symbol "
                                       << sym->getName() << " of unknown type\n";
                }
                relocPos += 4;
                continue;
              }

              // Patch the lis/addi pair
              if (patchLisAddiPair(code, relocPos, targetAddr, sym->getName())) {
                hasPatches = true;
                // Mark both lis and addi positions as patched
                patchedPositions.insert(relocPos);      // lis instruction
                patchedPositions.insert(relocPos + 4);  // addi instruction
              }
            } else if (config->verbose) {
              errorHandler().outs() << "  WARNING: Unexpected instruction at offset 0x"
                                   << utohexstr(relocPos) << ": 0x" << utohexstr(instruction)
                                   << " (opcode=" << (unsigned)instrOpcode << ")\n";
            }
          }

          // Advance by 4 from the ORIGINAL position (before any adjustment)
          relocPos = originalRelocPos + 4;
        } else if (opcode == PEF::kPEFRelocBySectD) {
          // BySectD: Data section reference (lis/addi or lis/stw pairs)
          // These reference symbols in the data section
          uint32_t runLength = operand + 1;

          if (config->verbose) {
            errorHandler().outs() << "    BySectD relocation: runLength=" << runLength
                                 << " at offset 0x" << utohexstr(relocPos) << "\n";
          }

          // Get data section base address
          OutputSection *dataSection = nullptr;
          for (OutputSection *osec : outputSections) {
            if (osec->getKind() == PEF::kPEFUnpackedDataSection) {
              dataSection = osec;
              break;
            }
          }

          if (!dataSection) {
            if (config->verbose) {
              errorHandler().outs() << "    WARNING: No data section found for BySectD relocation\n";
            }
            relocPos += runLength * 4;
            i++;
            continue;
          }

          uint64_t dataBaseAddr = dataSection->getVirtualAddress();

          // Process each relocation in the run
          for (uint32_t j = 0; j < runLength; j++) {  // Process each position in the run
            uint32_t currentPos = relocPos + (j * 4);
            uint32_t originalPos = currentPos;  // Track original position for marking as patched

            // Skip if already patched (happens when previous iteration patched a lis/addi pair)
            if (patchedPositions.count(currentPos)) {
              if (config->verbose) {
                errorHandler().outs() << "      BySectD at 0x" << utohexstr(currentPos)
                                     << " already patched, skipping\n";
              }
              continue;
            }

            // Check if this is a lis instruction
            if (currentPos + 3 < code.size()) {
              uint32_t instruction = (code[currentPos] << 24) |
                                    (code[currentPos + 1] << 16) |
                                    (code[currentPos + 2] << 8) |
                                     code[currentPos + 3];
              uint8_t instrOpcode = (instruction >> 26) & 0x3F;

              // SPECIAL CASE: CodeWarrior points relocations to immediate field (+2 bytes)
              if (instrOpcode == 0 && currentPos >= 2) {
                // Reading garbage means we're pointing to immediate field - adjust backwards
                uint32_t prevInstr = (code[currentPos - 2] << 24) |
                                     (code[currentPos - 1] << 16) |
                                     (code[currentPos] << 8) |
                                      code[currentPos + 1];
                uint8_t prevOpcode = (prevInstr >> 26) & 0x3F;

                // Adjust to instruction start regardless of opcode
                currentPos -= 2;
                instruction = prevInstr;
                instrOpcode = prevOpcode;

                if (config->verbose) {
                  errorHandler().outs() << "      BySectD adjusted -2 bytes (immediate field → instruction start), opcode="
                                       << (unsigned)instrOpcode << "\n";
                }
              }

              if (instrOpcode == 15) {  // lis instruction
                // This is the start of a lis/stw or lis/addi pair
                // Get the current offset from the addi/stw instruction
                if (currentPos + 7 < code.size()) {
                  uint32_t nextInstr = (code[currentPos + 4] << 24) |
                                      (code[currentPos + 5] << 16) |
                                      (code[currentPos + 6] << 8) |
                                       code[currentPos + 7];
                  int16_t currentOffset = (int16_t)(nextInstr & 0xFFFF);

                  // BUG FIX: Calculate offset in FINAL data section
                  // Final layout: [Import table][Function TVectors][Original data sections] (no separate main TVect)
                  uint32_t importTableSize = totalImportedSymbolCount * 4;
                  uint32_t adjustedOffset = importTableSize + functionTVectorsSize + currentOffset;

                  // Patch the pair with the adjusted offset
                  if (patchLisAddiPair(code, currentPos, adjustedOffset, "data section")) {
                    hasPatches = true;
                    // Mark BOTH the original relocation positions as patched (not the adjusted ones)
                    patchedPositions.insert(originalPos);
                    patchedPositions.insert(originalPos + 4);

                    if (config->verbose) {
                      errorHandler().outs() << "      Patched BySectD lis/addi pair at 0x"
                                           << utohexstr(currentPos) << " with adjusted offset 0x"
                                           << utohexstr(adjustedOffset) << " (import=" << importTableSize
                                           << " + funcTVects=" << functionTVectorsSize << " + original=" << currentOffset << ")\n";
                    }
                  }
                }
              } else if (instrOpcode == 14 || instrOpcode == 36 || instrOpcode == 32) {
                // addi (14), stw (36), or lwz (32) - patch the immediate offset
                // Extract current offset (symbol's offset in OBJECT FILE data section)
                int16_t currentOffset = (int16_t)(instruction & 0xFFFF);

                // BUG FIX: Calculate offset in FINAL data section
                // Final layout: [Import table][Function TVectors][Original data sections] (no separate main TVect)
                // Symbol's final offset = import table size + function TVectors size + original offset
                uint32_t importTableSize = totalImportedSymbolCount * 4;
                uint32_t adjustedOffset = importTableSize + functionTVectorsSize + currentOffset;

                // For runtime: r2 points to data section base, instruction needs section-relative offset
                uint16_t newOffset = adjustedOffset & 0xFFFF;

                // Patch the instruction
                uint32_t newInstr = (instruction & 0xFFFF0000) | newOffset;
                code[currentPos + 0] = (newInstr >> 24) & 0xFF;
                code[currentPos + 1] = (newInstr >> 16) & 0xFF;
                code[currentPos + 2] = (newInstr >> 8) & 0xFF;
                code[currentPos + 3] = newInstr & 0xFF;

                hasPatches = true;
                patchedPositions.insert(originalPos);

                if (config->verbose) {
                  const char *instrName = (instrOpcode == 14) ? "addi" :
                                         (instrOpcode == 36) ? "stw" : "lwz";
                  errorHandler().outs() << "      Patched BySectD " << instrName << " at 0x"
                                       << utohexstr(currentPos) << " with offset 0x"
                                       << utohexstr(newOffset) << " (base=0x"
                                       << utohexstr(dataBaseAddr) << " + current=0x"
                                       << utohexstr((uint16_t)currentOffset) << ")\n";
                }
              } else if (config->verbose) {
                errorHandler().outs() << "      BySectD at 0x" << utohexstr(currentPos)
                                     << " is not lis/addi/stw/lwz (opcode=" << (unsigned)instrOpcode << "), skipping\n";
              }
            }
          }

          relocPos += runLength * 4;
          i++;
        } else if (opcode == PEF::kPEFRelocBySectC) {
          // BySectC: Code section reference
          // CRITICAL: Function pointers must point to TVectors, not code addresses!
          // Patch addi instructions that take function addresses to use TVector addresses
          uint32_t runLength = operand + 1;

          if (config->verbose) {
            errorHandler().outs() << "    BySectC relocation: runLength=" << runLength
                                 << " at offset 0x" << utohexstr(relocPos) << "\n";
          }

          // Process each relocation in the run
          for (uint32_t j = 0; j < runLength; j++) {
            uint32_t currentPos = relocPos + (j * 4);

            // Skip if already patched
            if (patchedPositions.count(currentPos)) {
              continue;
            }

            // Check if this is an addi instruction at +2 offset (immediate field)
            if (currentPos >= 2 && currentPos + 1 < code.size()) {
              // Read potential addi instruction
              uint32_t instr = (code[currentPos - 2] << 24) |
                              (code[currentPos - 1] << 16) |
                              (code[currentPos] << 8) |
                              (code[currentPos + 1]);
              uint8_t instrOpcode = (instr >> 26) & 0x3F;

              // addi opcode is 14 (0xE)
              if (instrOpcode == 14) {
                // Extract immediate value (sign-extended 16-bit)
                uint16_t immediate = instr & 0xFFFF;

                // This immediate is a CODE section offset - look up which function
                // Find the symbol with this offset
                Defined *targetFunc = nullptr;
                for (const auto &entry : functionTVectors) {
                  Defined *func = cast<Defined>(entry.first);
                  if (func->getValue() == immediate) {
                    targetFunc = func;
                    break;
                  }
                }

                if (targetFunc) {
                  // Found the function - redirect to its TVector address
                  uint32_t tvectorOffset = functionTVectors[targetFunc];

                  // Get data section base
                  OutputSection *dataSection = nullptr;
                  for (OutputSection *osec : outputSections) {
                    if (isDataSection(osec->getKind())) {
                      dataSection = osec;
                      break;
                    }
                  }

                  if (dataSection) {
                    // Patch the immediate field with TVector offset (data-section-relative)
                    code[currentPos] = (tvectorOffset >> 8) & 0xFF;
                    code[currentPos + 1] = tvectorOffset & 0xFF;
                    hasPatches = true;
                    patchedPositions.insert(currentPos - 2);  // Mark instruction as patched

                    if (config->verbose) {
                      errorHandler().outs() << "      Redirected function pointer to TVector: "
                                           << targetFunc->getName() << " at offset 0x"
                                           << utohexstr(currentPos - 2)
                                           << " (code offset 0x" << utohexstr(immediate)
                                           << " → TVector offset 0x" << utohexstr(tvectorOffset) << ")\n";
                    }
                  }
                }
              }
            }
          }

          relocPos += runLength * 4;
          i++;

          /* OLD CODE - disabled, CFM handles this now - all code below is unreachable
          continue;

          // Get code section base address
          OutputSection *codeSection = nullptr;
          for (OutputSection *osec : outputSections) {
            if (osec->getKind() == PEF::kPEFCodeSection) {
              codeSection = osec;
              break;
            }
          }

          if (!codeSection) {
            if (config->verbose) {
              errorHandler().outs() << "    WARNING: No code section found for BySectC relocation\n";
            }
            relocPos += runLength * 4;
            i++;
            continue;
          }

          uint64_t codeBaseAddr = codeSection->getVirtualAddress();

          // Process each relocation in the run
          for (uint32_t j = 0; j < runLength; j++) {  // Process each position in the run
            uint32_t currentPos = relocPos + (j * 4);
            uint32_t originalPos = currentPos;  // Track original position for marking as patched

            // Skip if already patched (happens when previous iteration patched a lis/addi pair)
            if (patchedPositions.count(currentPos)) {
              if (config->verbose) {
                errorHandler().outs() << "      BySectC at 0x" << utohexstr(currentPos)
                                     << " already patched, skipping\n";
              }
              continue;
            }

            // Check if this is a lis instruction
            if (currentPos + 3 < code.size()) {
              uint32_t instruction = (code[currentPos] << 24) |
                                    (code[currentPos + 1] << 16) |
                                    (code[currentPos + 2] << 8) |
                                     code[currentPos + 3];
              uint8_t instrOpcode = (instruction >> 26) & 0x3F;

              // SPECIAL CASE: CodeWarrior points relocations to immediate field (+2 bytes)
              if (instrOpcode == 0 && currentPos >= 2) {
                // Try reading 2 bytes earlier to check for lis
                uint32_t prevInstr = (code[currentPos - 2] << 24) |
                                     (code[currentPos - 1] << 16) |
                                     (code[currentPos] << 8) |
                                      code[currentPos + 1];
                uint8_t prevOpcode = (prevInstr >> 26) & 0x3F;

                if (prevOpcode == 15) {  // Found lis instruction
                  // Adjust to instruction start
                  currentPos -= 2;
                  instruction = prevInstr;
                  instrOpcode = 15;

                  if (config->verbose) {
                    errorHandler().outs() << "      BySectC adjusted -2 bytes (immediate field → instruction start)\n";
                  }
                }
              }

              if (instrOpcode == 15) {  // lis instruction
                // This is the start of a lis/addi pair
                // Patch the pair with code section base address
                if (patchLisAddiPair(code, currentPos, codeBaseAddr, "code section")) {
                  hasPatches = true;
                  // Mark BOTH the original relocation positions as patched (not the adjusted ones)
                  patchedPositions.insert(originalPos);
                  patchedPositions.insert(originalPos + 4);

                  if (config->verbose) {
                    errorHandler().outs() << "      Patched BySectC lis/addi pair at 0x"
                                         << utohexstr(currentPos) << " with code base 0x"
                                         << utohexstr(codeBaseAddr) << "\n";
                  }
                }
              } else if (instrOpcode == 14 || instrOpcode == 36 || instrOpcode == 32) {
                // addi (14), stw (36), or lwz (32) - patch the immediate offset
                // Extract current offset and add section base
                int16_t currentOffset = (int16_t)(instruction & 0xFFFF);
                uint64_t finalAddr = codeBaseAddr + currentOffset;
                uint16_t newOffset = finalAddr & 0xFFFF;

                // Patch the instruction
                uint32_t newInstr = (instruction & 0xFFFF0000) | newOffset;
                code[currentPos + 0] = (newInstr >> 24) & 0xFF;
                code[currentPos + 1] = (newInstr >> 16) & 0xFF;
                code[currentPos + 2] = (newInstr >> 8) & 0xFF;
                code[currentPos + 3] = newInstr & 0xFF;

                hasPatches = true;
                patchedPositions.insert(originalPos);

                if (config->verbose) {
                  const char *instrName = (instrOpcode == 14) ? "addi" :
                                         (instrOpcode == 36) ? "stw" : "lwz";
                  errorHandler().outs() << "      Patched BySectC " << instrName << " at 0x"
                                       << utohexstr(currentPos) << " with offset 0x"
                                       << utohexstr(newOffset) << " (base=0x"
                                       << utohexstr(codeBaseAddr) << " + current=0x"
                                       << utohexstr((uint16_t)currentOffset) << ")\n";
                }
              } else if (config->verbose) {
                errorHandler().outs() << "      BySectC at 0x" << utohexstr(currentPos)
                                     << " is not lis/addi/stw/lwz (opcode=" << (unsigned)instrOpcode << "), skipping\n";
              }
            }
          }

          relocPos += runLength * 4;
          i++;
          */ // END OLD CODE - commented out
        } else {
          // Other relocation types - skip
          relocPos += 4;
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

void Writer::optimizeTOCRestores() {
  if (config->verbose) {
    errorHandler().outs() << "\nOptimizing TOC restores for Mac OS Classic CFM...\n";
  }

  // This optimization removes unnecessary TOC restores (`lwz r2, 20(r1)`)
  // that follow calls to functions in the same PEF fragment.
  //
  // Background: The compiler conservatively emits `bl <target>; lwz r2, 20(r1)`
  // for all calls that might cross fragment boundaries. However, after linking,
  // most calls are within the same fragment and don't need TOC restore.
  //
  // Strategy: Scan for `bl <stub>` patterns. If the bl targets the import stub
  // region, keep the TOC restore. Otherwise (direct call to local function),
  // replace the TOC restore with nop.
  //
  // This prevents loading r2 from uninitialized stack locations and
  // improves code size/performance.

  int sameFragmentOptimized = 0;
  int stubCallsKept = 0;
  int totalTOCRestores = 0;

  // Find the code section and determine stub region
  OutputSection *codeSection = nullptr;
  for (OutputSection *osec : outputSections) {
    if (osec->getKind() == PEF::kPEFCodeSection) {
      codeSection = osec;
      break;
    }
  }

  if (!codeSection) {
    if (config->verbose) {
      errorHandler().outs() << "  No code section found, skipping TOC optimization\n";
    }
    return;
  }

  // Stub region starts after the original code
  uint32_t stubRegionStart = codeSection->getOriginalSize();
  uint32_t codeBaseVA = codeSection->getVirtualAddress();

  if (config->verbose) {
    errorHandler().outs() << "  Code section VA: 0x" << utohexstr(codeBaseVA) << "\n";
    errorHandler().outs() << "  Stub region starts at offset: 0x" << utohexstr(stubRegionStart) << "\n";
  }

  for (OutputSection *osec : outputSections) {
    if (osec->getKind() != PEF::kPEFCodeSection)
      continue;

    for (InputSection *isec : osec->getInputSections()) {
      // Check for patched code in priority order:
      // 1. Writer's patchedCode map (from replaceImportCalls)
      // 2. InputSection's patched data (from processRelocations)
      // 3. Original data from input file
      std::vector<uint8_t> code;
      auto patchedIt = patchedCode.find(isec);
      if (patchedIt != patchedCode.end()) {
        // Use patched code from import stub replacement
        code = patchedIt->second;
      } else if (isec->hasPatchedData()) {
        ArrayRef<uint8_t> data = isec->getPatchedData();
        code = std::vector<uint8_t>(data.begin(), data.end());
      } else {
        auto dataOrErr = isec->getData();
        if (!dataOrErr) {
          error("failed to get code section data: " + toString(dataOrErr.takeError()));
          continue;
        }
        code = std::vector<uint8_t>(dataOrErr->begin(), dataOrErr->end());
      }

      bool modified = false;

      uint32_t isecOffset = isec->getVirtualAddress() - codeBaseVA;

      // Scan for `bl <target>; lwz r2, 20(r1)` sequences
      for (size_t offset = 0; offset + 7 < code.size(); offset += 4) {
        uint32_t instr1 = endian::read32be(&code[offset]);
        uint32_t instr2 = endian::read32be(&code[offset + 4]);

        // Check for `bl` (opcode 18, AA=0, LK=1)
        bool isBL = (instr1 & 0xFC000003) == 0x48000001;

        // Check for `lwz r2, 20(r1)` (opcode 32, rD=2, rA=1, d=20)
        bool isTOCRestore = (instr2 == 0x80410014);

        if (isBL && isTOCRestore) {
          totalTOCRestores++;

          // Calculate branch target
          int32_t displacement = (instr1 & 0x03FFFFFC);
          // Sign extend 26-bit value
          if (displacement & 0x02000000) {
            displacement |= 0xFC000000;
          }

          // Target VA = current VA + displacement
          uint32_t currentVA = isecOffset + offset;
          uint32_t targetVA = currentVA + displacement;

          // Check if target is in stub region
          bool isStubCall = (targetVA >= stubRegionStart);

          if (isStubCall) {
            // Call to import stub - keep TOC restore
            stubCallsKept++;

            if (config->verbose) {
              errorHandler().outs() << "    Keeping TOC restore for stub call at 0x"
                                   << utohexstr(currentVA) << " -> 0x" << utohexstr(targetVA) << "\n";
            }
          } else {
            // Direct call to local function - remove TOC restore
            endian::write32be(&code[offset + 4], 0x60000000); // nop
            modified = true;
            sameFragmentOptimized++;

            if (config->verbose) {
              errorHandler().outs() << "    Optimized local call at 0x"
                                   << utohexstr(currentVA) << " -> 0x" << utohexstr(targetVA) << "\n";
            }
          }
        }
      }

      // Update the section data if we made changes
      if (modified) {
        patchedCode[isec] = std::move(code);
      }
    }
  }

  if (config->verbose || true) {  // Always show summary
    errorHandler().outs() << "  TOC restore optimization summary:\n";
    errorHandler().outs() << "    Total TOC restores found: " << totalTOCRestores << "\n";
    errorHandler().outs() << "    Same-fragment calls optimized: " << sameFragmentOptimized << "\n";
    errorHandler().outs() << "    Stub calls kept: " << stubCallsKept << "\n";
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

  // Temporary file offset assignment to get section base addresses for virtual address calculation
  assignFileOffsets();

  // Assign virtual addresses to all defined symbols (MUST be before createFunctionTVectors!)
  // The TVector sorting needs valid virtual addresses to order functions correctly
  assignSymbolAddresses();

  // Create TVectors for all defined functions (for function pointers)
  // Must be done after assignSymbolAddresses() so sort order is correct
  // This populates codeFunctions vector needed for TVector writing
  createFunctionTVectors();

  // Re-assign file offsets now that we know the TVector table size
  // This writes the actual TVector data using the sorted codeFunctions vector
  finalFileOffsetPass = true;  // Set flag so data section encoding happens with correct addresses
  assignFileOffsets();

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

  // Optimize TOC restores for same-fragment calls (Mac OS Classic CFM)
  optimizeTOCRestores();

  // BUG FIX #15: Create loader section AFTER tvectOffset is finalized
  createLoaderSection();

  // BUG FIX: Calculate final file size accounting for new layout
  // Loader section is now FIRST (after headers), so file size is determined by
  // the last section (code or data), not loader
  // fileSize was already set in assignFileOffsets() to include all code/data sections
  // Just verify loader section fits in its reserved space
  if (loaderData.size() > 256) {
    error("Loader section size (" + Twine(loaderData.size()) +
          " bytes) exceeds reserved space (256 bytes). Increase estimatedLoaderSize.");
  }

  // BUG FIX: Adjust section offsets if loader section is smaller than estimated
  // assignFileOffsets() reserved 256 bytes for loader, but actual size may be less
  uint32_t actualLoaderSize = loaderData.size();
  uint32_t estimatedLoaderSize = 256;
  if (actualLoaderSize < estimatedLoaderSize) {
    int32_t offsetAdjustment = actualLoaderSize - estimatedLoaderSize;
    // Align actual loader end to next 16-byte boundary
    uint32_t loaderEnd = loaderSectionOffset + actualLoaderSize;
    uint32_t alignedLoaderEnd = alignTo(loaderEnd, 16);
    offsetAdjustment = alignedLoaderEnd - (loaderSectionOffset + estimatedLoaderSize);

    if (config->verbose) {
      errorHandler().outs() << "Adjusting section offsets: loader size=" << actualLoaderSize
                           << " bytes (estimated " << estimatedLoaderSize
                           << "), adjustment=" << offsetAdjustment << " bytes\n";
    }

    // Recalculate section offsets starting from aligned loader end
    uint64_t newOffset = alignedLoaderEnd;
    uint64_t lastSectionEnd = 0;
    for (OutputSection *osec : outputSections) {
      uint64_t oldOffset = osec->getFileOffset();
      if (oldOffset > loaderSectionOffset) {
        osec->setFileOffset(newOffset);
        if (config->verbose) {
          errorHandler().outs() << "  " << osec->getName()
                               << ": 0x" << utohexstr(oldOffset)
                               << " -> 0x" << utohexstr(newOffset) << "\n";
        }
        // Track the end of the last section
        // For pattern-encoded sections, use encoded size; otherwise use section size
        uint64_t sectionFileSize = osec->hasEncodedData() ? osec->getEncodedData().size() : osec->getSize();
        uint64_t sectionEnd = newOffset + sectionFileSize;
        if (sectionEnd > lastSectionEnd) {
          lastSectionEnd = sectionEnd;
        }
        // Advance to next section with 16-byte alignment (using total size for offset calculation)
        newOffset += osec->getSize();
        newOffset = alignTo(newOffset, 16);
      }
    }

    // Recalculate file size based on last section end
    fileSize = lastSectionEnd;
  }

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
