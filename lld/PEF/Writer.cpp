//===- Writer.cpp ---------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Writer.h"
#include "Config.h"
#include "ELFInputFiles.h"
#include "InputFiles.h"
#include "InputSection.h"
#include "OutputSection.h"
#include "PatternEncoder.h"
#include "RelocWriter.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "lld/Common/ErrorHandler.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/PEF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileOutputBuffer.h"
#include "llvm/Support/MathExtras.h"
#include <algorithm>
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
  void recalculateFileOffsets();  // Recalculate offsets after loader section is created
  void createLoaderSection();
  void collectImports();
  void createEntryPointTVect();
  void updateEntryPointTVect();  // Update TVect TOC address after collectImports
  void collectFunctions();        // Collect and sort all code functions
  void collectAddressTakenFunctions();  // Collect functions with addresses taken
  void createFunctionTVectors();  // Create TVector offset map for functions
  void patchVTableEntries();      // Patch vtable function pointers to point to TVectors
  void generateImportStubs();     // Generate stubs in code section
  void generateImportStubsM68k(); // Generate M68k CFM-68K stubs (16 bytes each)
  void generateImportStubsPPC();  // Generate PowerPC stubs (24 bytes each)
  void generateTOCEntries();      // Generate TOC entries in data section
  void replaceImportCalls();      // Replace bl .+1 with calls to import stubs
  void processELFRelocations();   // Process ELF R_PPC_REL24 relocations
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
  uint32_t loaderSizeForLayout = 0;  // Size used during offset assignment (0 initially, set after createLoaderSection)

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

  // Sparse TVector: Only create TVectors for functions whose addresses are taken
  // This prevents TOC overflow for large binaries (Rust core, etc.)
  // Use name-based tracking to handle symbol identity mismatches (file-local symbols
  // like .text have different object instances in addressTakenFunctions vs getSymbols())
  DenseSet<StringRef> globalAddressTakenFunctionNames;

  // VTABLE FIX: Track code addresses from vtables that need TVectors
  // These are anonymous/local functions without global symbols
  // Maps code virtual address -> TVector offset in data section
  DenseMap<uint64_t, uint32_t> vtableCodeAddressToTVector;
  std::set<uint64_t> vtableCodeAddresses;  // Sorted set of vtable function code addresses

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

  // Track data section offset for each InputFile (for BySectD patching)
  DenseMap<InputFile*, uint64_t> objFileDataOffsets;

  // Track positions in code section that have been patched by replaceImportCalls()
  // Positions are stored relative to INPUT sections (not output section)
  std::set<uint32_t> patchedPositions;

  // Track vtable function pointer relocations that need BySectD
  // These are collected during processELFRelocations and passed to RelocWriter
  std::vector<VTableRelocation> vtableRelocations;

  // Track the actual output offset of each InputSection in the data section
  // This is populated during assignFileOffsets() after section reordering
  DenseMap<InputSection*, uint32_t> inputSectionOutputOffsets;

  // Track pending data-to-data relocations that need to be patched during layout
  // These point from one data section to another (e.g., &str slices pointing to .rodata strings)
  struct DataToDataReloc {
    InputSection *sourceSection;   // Section containing the pointer
    uint32_t sourceOffset;         // Offset within source section
    InputSection *targetSection;   // Section being pointed to
    uint64_t targetAddend;         // Addend to add to target section's position
  };
  std::vector<DataToDataReloc> pendingDataToDataRelocs;
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
  // loaderSizeForLayout is 0 on first passes, then set to actual size after createLoaderSection()
  // This allows two-pass layout: first pass with 0, recalculate after loader is built
  offset += loaderSizeForLayout;

  if (config->verbose) {
    errorHandler().outs() << "Loader section offset: 0x" << utohexstr(loaderSectionOffset)
                         << ", layout size: " << loaderSizeForLayout << " bytes\n";
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

      // Data section layout: [Import table][Padding (8 bytes)][Function TVectors (8 bytes each)][User data]
      // FIX: Allocate space for ALL function TVectors, not just entry point
      // On first pass, functionTVectorsSize is 0 (codeFunctions not populated yet)
      // On second pass (after createFunctionTVectors), use actual size
      uint32_t paddingSize = 8;  // CodeWarrior adds 8 bytes padding before TVector
      uint32_t tvectorTableSize = (functionTVectorsSize > 0) ? functionTVectorsSize : 8;  // Min 8 for entry point
      sectionSize += importTableSize + paddingSize + tvectorTableSize;

      if (config->verbose && (importTableSize > 0 || tvectorTableSize > 0)) {
        errorHandler().outs() << "Data section additions (TVector8 model):\n"
                             << "  Import table: " << importTableSize << " bytes\n"
                             << "  Padding: " << paddingSize << " bytes\n"
                             << "  Function TVectors: " << tvectorTableSize << " bytes\n"
                             << "  Total: " << (importTableSize + paddingSize + tvectorTableSize) << " bytes\n"
                             << "  (No TOC entries - using direct import access)\n";
      }
    }

    // If this is the code section, reserve space for import stubs
    if (osec->getKind() == PEF::kPEFCodeSection && totalImportedSymbolCount > 0) {
      // BUG FIX #35: Import stubs size varies by architecture
      // PowerPC: 24 bytes per import (matching CodeWarrior)
      // M68k: 16 bytes per import
      uint32_t stubBytesPerImport = (config->architecture == PEFArch::M68k) ? 16 : 24;
      uint32_t stubsSize = totalImportedSymbolCount * stubBytesPerImport;
      sectionSize += stubsSize;

      if (config->verbose) {
        errorHandler().outs() << "Code section additions:\n"
                             << "  Import stubs: " << stubsSize << " bytes ("
                             << totalImportedSymbolCount << " imports × "
                             << stubBytesPerImport << " bytes)\n";
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

      // FIX: Write TVectors for ALL defined functions, not just entry point
      // This enables function pointers (e.g., atexit handlers) to work correctly

      // Find code section to get its base address for calculating section-relative offsets
      OutputSection *codeSection = nullptr;
      for (OutputSection *sec : outputSections) {
        if (sec->getKind() == PEF::kPEFCodeSection) {
          codeSection = sec;
          break;
        }
      }
      uint64_t codeBaseVA = codeSection ? codeSection->getVirtualAddress() : 0;

      // Write TVectors for ALL functions in sorted order
      // codeFunctions is populated by createFunctionTVectors() which is called before the second pass
      if (!codeFunctions.empty() && finalFileOffsetPass) {
        for (Defined *func : codeFunctions) {
          // Use getVirtualAddress() which returns final linked address
          // getValue() returns 0 for unassigned symbols, causing TVectors to be all zeros
          // CFM will add section base address via TVector8 relocations at load time
          uint32_t codeAddress = func->getVirtualAddress() - codeBaseVA;  // Section-relative offset
          uint32_t tocAddress = 0;  // r2 points to data section start (CodeWarrior model)

          // Write TVector8 as 8 bytes (big-endian) - no environment field
          uint8_t tvectorBytes[8];
          write32be(tvectorBytes + 0, codeAddress);
          write32be(tvectorBytes + 4, tocAddress);

          dataContent.insert(dataContent.end(), tvectorBytes, tvectorBytes + 8);

          if (config->verbose) {
            errorHandler().outs() << "  Writing TVector8 for " << func->getName()
                                 << ": code=0x" << utohexstr(codeAddress)
                                 << " at data offset 0x" << utohexstr(functionTVectors[func])
                                 << "\n";
          }
        }

        // VTABLE FIX: Write TVectors for anonymous vtable functions
        // These are functions without global symbols, detected from R_PPC_ADDR32 relocations in data sections
        // Note: vtableCodeAddresses contains virtual addresses (VAs), which must be
        // converted to section-relative offsets by subtracting codeBaseVA.
        for (uint64_t codeVA : vtableCodeAddresses) {
          // Convert VA to section-relative offset (matching named function handling at line 313)
          uint32_t codeAddress = static_cast<uint32_t>(codeVA - codeBaseVA);
          uint32_t tocAddress = 0;  // r2 points to data section start (CodeWarrior model)

          // Write TVector8 as 8 bytes (big-endian) - no environment field
          uint8_t tvectorBytes[8];
          write32be(tvectorBytes + 0, codeAddress);
          write32be(tvectorBytes + 4, tocAddress);

          dataContent.insert(dataContent.end(), tvectorBytes, tvectorBytes + 8);

          if (config->verbose) {
            errorHandler().outs() << "  Writing TVector8 for anonymous function VA 0x"
                                 << utohexstr(codeVA) << " (code offset 0x" << utohexstr(codeAddress) << ")"
                                 << " at data offset 0x" << utohexstr(vtableCodeAddressToTVector[codeVA])
                                 << "\n";
          }
        }
      } else if (!vtableCodeAddresses.empty() && finalFileOffsetPass) {
        // VTABLE FIX: Write TVectors for anonymous vtable functions even if no named functions
        // Note: vtableCodeAddresses contains virtual addresses (VAs)
        for (uint64_t codeVA : vtableCodeAddresses) {
          // Convert VA to section-relative offset
          uint32_t codeAddress = static_cast<uint32_t>(codeVA - codeBaseVA);
          uint32_t tocAddress = 0;

          uint8_t tvectorBytes[8];
          write32be(tvectorBytes + 0, codeAddress);
          write32be(tvectorBytes + 4, tocAddress);

          dataContent.insert(dataContent.end(), tvectorBytes, tvectorBytes + 8);

          if (config->verbose) {
            errorHandler().outs() << "  Writing TVector8 for anonymous function VA 0x"
                                 << utohexstr(codeVA) << " (code offset 0x" << utohexstr(codeAddress) << ")"
                                 << " at data offset 0x" << utohexstr(vtableCodeAddressToTVector[codeVA])
                                 << "\n";
          }
        }
      } else {
        // First pass or no functions: write placeholder for at least one TVector (entry point)
        // This ensures section size is consistent between passes
        uint8_t placeholderTVector[8] = {0};
        dataContent.insert(dataContent.end(), placeholderTVector, placeholderTVector + 8);
      }

      // BUG FIX: Include original input section data (e.g., global variables)
      // This was missing, causing global variables to not be included in the output!
      // CRITICAL: Use patched data if available (after relocations are processed)

      // TOC OVERFLOW FIX: Sort input sections to keep small data within 32KB range
      // Strategy: Place sections with relocations first (they need TOC access),
      // then sort by size (smallest first). This ensures frequently-accessed small
      // data and pointers are within the 16-bit signed offset limit from r2.
      // Large static tables (like Rust's POWER_OF_FIVE_128) go at the end.
      std::vector<InputSection *> sortedDataSections;
      for (InputSection *isec : osec->getInputSections()) {
        sortedDataSections.push_back(isec);
      }

      std::sort(sortedDataSections.begin(), sortedDataSections.end(),
        [](InputSection *a, InputSection *b) {
          // Sections with relocations come first (they need TOC-relative access)
          bool aHasRelocs = !a->getRelocations().empty();
          bool bHasRelocs = !b->getRelocations().empty();
          if (aHasRelocs != bHasRelocs)
            return aHasRelocs;  // Sections with relocs first

          // Then sort by size (smallest first to keep them within 32KB range)
          return a->getSize() < b->getSize();
        });

      if (config->verbose && sortedDataSections.size() > 1) {
        errorHandler().outs() << "  Data section ordering (TOC overflow prevention):\n";
        uint32_t runningOffset = dataContent.size();
        for (InputSection *isec : sortedDataSections) {
          bool hasRelocs = !isec->getRelocations().empty();
          errorHandler().outs() << "    offset=0x" << utohexstr(runningOffset)
                               << " size=" << isec->getSize()
                               << " relocs=" << (hasRelocs ? "yes" : "no")
                               << " from " << isec->getFile()->getName() << "\n";
          runningOffset += isec->getSize();
        }
      }

      for (InputSection *isec : sortedDataSections) {
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

        // BUG FIX: Ensure 4-byte alignment for each input section
        // This is critical for BySectD relocations which patch 4-byte aligned values.
        // Without alignment padding, vtable pointers may end up at misaligned offsets,
        // causing CFM to patch incorrect bytes at runtime.
        size_t currentSize = dataContent.size();
        size_t alignment = 4;  // PEF uses 4-byte alignment for data
        size_t alignedSize = (currentSize + alignment - 1) & ~(alignment - 1);
        if (alignedSize > currentSize) {
          size_t paddingNeeded = alignedSize - currentSize;
          dataContent.insert(dataContent.end(), paddingNeeded, 0);
          if (config->verbose) {
            errorHandler().outs() << "  Added " << paddingNeeded
                                 << " alignment padding bytes\n";
          }
        }

        dataContent.insert(dataContent.end(), inputData.begin(), inputData.end());

        // Track this ObjFile's data section offset for BySectD patching
        // We need this information when processing BySectD relocations in replaceImportCalls
        // Calculate where this input section's data starts in dataContent
        // NOTE: dataContent already contains the prepend (import table + padding + TVector)
        // at this point, so inputSectionStart already includes the prepend offset
        // BUG FIX: Only record on final pass when Entry TVector is actually written!
        // On first pass, Entry TVector is NOT written (entryFunc not found or VA not assigned),
        // so the offsets would be 8 bytes too small.
        uint32_t inputSectionStart = dataContent.size() - inputData.size();

        // CRITICAL FIX: Update input section VA to reflect actual position after reordering
        // The sections were sorted for TOC overflow prevention, so their VAs from finalizeLayout()
        // are now stale. Update to the correct offset from data section start.
        // dataSectionVA = osec->getVirtualAddress() (the output section's VA)
        // inputSectionStart = offset from data section start (includes import table + padding + TVectors)
        isec->setVirtualAddress(osec->getVirtualAddress() + inputSectionStart);

        InputFile *objFile = isec->getFile();
        if (objFile && finalFileOffsetPass) {
          // Record/update on final pass - Entry TVector is now written
          // The inputSectionStart correctly accounts for prepend since dataContent
          // now contains [Import table][Padding][Entry TVector] before user data
          objFileDataOffsets[objFile] = inputSectionStart;
          if (config->verbose) {
            errorHandler().outs() << "  Recording data offset for " << objFile->getName()
                                 << ": 0x" << utohexstr(inputSectionStart) << "\n";
          }
        }

        // Track actual output offset for each InputSection (for vtable BySectD relocations)
        // This is needed because sections are reordered for TOC overflow prevention,
        // so the offset computed during patchVTableEntries() would be wrong.
        if (finalFileOffsetPass) {
          inputSectionOutputOffsets[isec] = inputSectionStart;
        }

        // BUG FIX: Adjust internal data pointers for section prepend
        // When the linker prepends import table + padding + TVectors to user data,
        // pointer values in the data section need to be adjusted to account for
        // this offset. BySectD relocations tell CFM to add the section base,
        // but the raw pointer values must already include the offset within the section.
        // IMPORTANT: Only do this on the final pass to avoid double adjustment
        // BUG FIX #36: Use functionTVectorsSize (ALL function TVectors), not just 8 bytes
        // This was causing user data to overlap with function TVectors when there are
        // multiple functions, because sectionPrepend didn't reserve enough space.
        uint32_t sectionPrepend = importTableSize + paddingSize + functionTVectorsSize;
        if (sectionPrepend > 0 && finalFileOffsetPass) {
          ArrayRef<uint16_t> relocInstrs = isec->getRelocations();
          uint32_t relocAddress = 0;

          for (size_t ri = 0; ri < relocInstrs.size(); ++ri) {
            uint16_t instr = support::endian::read16be(&relocInstrs[ri]);
            uint8_t opcode = (instr >> 9) & 0x7F;
            uint16_t operand = instr & 0x1FF;

            using namespace llvm::PEF;

            if (opcode == kPEFRelocBySectD) {
              // BySectD: relocate runLength words at current cursor
              uint32_t runLength = operand + 1;

              for (uint32_t j = 0; j < runLength; ++j) {
                uint32_t ptrOffset = inputSectionStart + relocAddress + (j * 4);

                if (ptrOffset + 4 <= dataContent.size()) {
                  // Read current pointer value (big-endian)
                  uint32_t ptrValue = (static_cast<uint32_t>(dataContent[ptrOffset]) << 24) |
                                     (static_cast<uint32_t>(dataContent[ptrOffset + 1]) << 16) |
                                     (static_cast<uint32_t>(dataContent[ptrOffset + 2]) << 8) |
                                     static_cast<uint32_t>(dataContent[ptrOffset + 3]);

                  // Adjust by section prepend size
                  uint32_t adjustedValue = ptrValue + sectionPrepend;

                  // Write back (big-endian)
                  dataContent[ptrOffset] = (adjustedValue >> 24) & 0xFF;
                  dataContent[ptrOffset + 1] = (adjustedValue >> 16) & 0xFF;
                  dataContent[ptrOffset + 2] = (adjustedValue >> 8) & 0xFF;
                  dataContent[ptrOffset + 3] = adjustedValue & 0xFF;

                  if (config->verbose) {
                    errorHandler().outs() << "    Adjusted BySectD pointer at offset 0x"
                                         << utohexstr(ptrOffset) << ": 0x"
                                         << utohexstr(ptrValue) << " -> 0x"
                                         << utohexstr(adjustedValue) << "\n";
                  }
                }
              }

              relocAddress += runLength * 4;
            } else if (opcode == kPEFRelocSetPosition) {
              // SetPosition: 2-instruction sequence to set cursor
              if (ri + 1 < relocInstrs.size()) {
                uint16_t instr2 = support::endian::read16be(&relocInstrs[++ri]);
                relocAddress = (operand << 16) | instr2;
              }
            } else if (opcode == kPEFRelocIncrPosition) {
              // IncrPosition: advance cursor by (operand + 1) bytes
              relocAddress += operand + 1;
            } else if (opcode == kPEFRelocBySectC) {
              // BySectC: just advance cursor
              uint32_t runLength = operand + 1;
              relocAddress += runLength * 4;
            }
            // Other opcodes: skip for now
          }
        }
      }

      // Only encode data on the final pass when function addresses are known
      if (finalFileOffsetPass) {
        // DATA-TO-DATA RELOCATION FIX: Apply deferred patches now that we know section positions
        // pendingDataToDataRelocs was populated in patchVTableEntries(), but we couldn't
        // compute correct offsets until sections were laid out.
        if (!pendingDataToDataRelocs.empty()) {
          if (config->verbose) {
            errorHandler().outs() << "Applying " << pendingDataToDataRelocs.size()
                                 << " deferred data-to-data relocations...\n";
          }

          for (const auto &reloc : pendingDataToDataRelocs) {
            // Look up the output offset of source and target sections
            auto sourceIt = inputSectionOutputOffsets.find(reloc.sourceSection);
            auto targetIt = inputSectionOutputOffsets.find(reloc.targetSection);

            if (sourceIt == inputSectionOutputOffsets.end()) {
              if (config->verbose) {
                errorHandler().outs() << "  WARNING: Source section " << reloc.sourceSection->getName()
                                     << " not found in offset map\n";
              }
              continue;
            }

            if (targetIt == inputSectionOutputOffsets.end()) {
              if (config->verbose) {
                errorHandler().outs() << "  WARNING: Target section " << reloc.targetSection->getName()
                                     << " not found in offset map\n";
              }
              continue;
            }

            // Calculate the pointer value: target section position + addend
            uint32_t targetOffset = targetIt->second + reloc.targetAddend;

            // Calculate where in dataContent to patch: source section position + reloc offset
            uint32_t patchPosition = sourceIt->second + reloc.sourceOffset;

            if (patchPosition + 4 <= dataContent.size()) {
              // Patch the pointer value (big-endian)
              dataContent[patchPosition + 0] = (targetOffset >> 24) & 0xFF;
              dataContent[patchPosition + 1] = (targetOffset >> 16) & 0xFF;
              dataContent[patchPosition + 2] = (targetOffset >> 8) & 0xFF;
              dataContent[patchPosition + 3] = targetOffset & 0xFF;

              if (config->verbose) {
                errorHandler().outs() << "  Patched data-to-data at 0x" << utohexstr(patchPosition)
                                     << " (source " << reloc.sourceSection->getName() << "+0x"
                                     << utohexstr(reloc.sourceOffset) << ")"
                                     << " -> 0x" << utohexstr(targetOffset)
                                     << " (target " << reloc.targetSection->getName() << "+0x"
                                     << utohexstr(reloc.targetAddend) << ")\n";
              }
            } else {
              if (config->verbose) {
                errorHandler().outs() << "  ERROR: Patch position 0x" << utohexstr(patchPosition)
                                     << " out of bounds (dataContent size=" << dataContent.size() << ")\n";
              }
            }
          }

          // Clear to avoid re-processing if assignFileOffsets is called again
          pendingDataToDataRelocs.clear();
        }

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

void Writer::recalculateFileOffsets() {
  // Recalculate all file offsets after loader section is created
  // This is called after createLoaderSection() to use the actual loader size
  uint32_t actualLoaderSize = loaderData.size();

  if (config->verbose) {
    errorHandler().outs() << "\nRecalculating file offsets with actual loader size: "
                         << actualLoaderSize << " bytes\n";
  }

  // Count non-empty sections (same logic as assignFileOffsets)
  int nonEmptySections = 0;
  for (OutputSection *osec : outputSections) {
    bool hasTVect = (tvectSectionIndex >= 0 &&
                     osec == outputSections[tvectSectionIndex] &&
                     !tvectData.empty());
    if (!osec->getInputSections().empty() || hasTVect)
      nonEmptySections++;
  }

  // Start after container header (40 bytes) + section headers (28 bytes each)
  // +1 for loader section header
  uint32_t offset = 40;  // Container header size
  offset += (nonEmptySections + 1) * PEF::kSectionHeaderFileSize;
  offset = alignTo(offset, 16);

  // Loader section comes first
  loaderSectionOffset = offset;
  offset += actualLoaderSize;
  offset = alignTo(offset, 16);

  // Recalculate offsets for each non-empty output section
  uint64_t lastSectionEnd = 0;
  for (OutputSection *osec : outputSections) {
    bool hasTVect = (tvectSectionIndex >= 0 &&
                     osec == outputSections[tvectSectionIndex] &&
                     !tvectData.empty());
    if (osec->getInputSections().empty() && !hasTVect)
      continue;

    uint64_t oldOffset = osec->getFileOffset();
    osec->setFileOffset(offset);

    if (config->verbose) {
      errorHandler().outs() << "  " << osec->getName()
                           << ": 0x" << utohexstr(oldOffset)
                           << " -> 0x" << utohexstr(offset) << "\n";
    }

    // Calculate section file size (use encoded data if available)
    uint64_t sectionFileSize = osec->hasEncodedData()
                                   ? osec->getEncodedData().size()
                                   : osec->getSize();
    uint64_t sectionEnd = offset + sectionFileSize;
    if (sectionEnd > lastSectionEnd) {
      lastSectionEnd = sectionEnd;
    }

    // Advance to next section with alignment
    offset += osec->getSize();
    offset = alignTo(offset, 16);
  }

  // Update file size
  fileSize = lastSectionEnd;

  if (config->verbose) {
    errorHandler().outs() << "  Final file size: " << fileSize << " bytes\n";
  }
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

    // BUG FIX: For data sections, account for import table, padding, and TVectors that come before original data
    // Layout: [Import table][Padding (8 bytes)][Function TVectors (8 bytes each)][User data]
    uint64_t dataOffset = 0;
    if (isDataSection(osec->getKind())) {
      uint32_t importTableSize = totalImportedSymbolCount * 4;
      uint32_t paddingSize = 8;  // CodeWarrior adds 8 bytes padding before TVector
      // On first pass (before createFunctionTVectors), functionTVectorsSize is 0
      // Use minimum of 8 bytes for entry point TVector in that case
      uint32_t tvectorTableSize = (functionTVectorsSize > 0) ? functionTVectorsSize : 8;
      dataOffset = importTableSize + paddingSize + tvectorTableSize;
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
  PEFRelocWriter relocWriter(outputSections, importedLibraries, functionTVectorsSize,
                             &patchedPositions, &vtableRelocations,
                             &inputSectionOutputOffsets);
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

  for (const auto &entry : functionTVectors) {
    Defined *func = cast<Defined>(entry.first);
    if (func->getName() == config->entry) {
      entryFunc = func;
      // FIX: Use actual offset from functionTVectors map
      // The map contains the correct offset for each function's TVector
      entryTVectorOffset = entry.second;
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

  // Architecture tag based on target
  uint32_t archTag = (config->architecture == PEFArch::M68k)
                         ? PEF::kPEFM68KArch     // 'm68k'
                         : PEF::kPEFPowerPCArch; // 'pwpc'
  write32be(buf + 8, archTag);
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
      // 1. Writer's patchedCode map (from replaceImportCalls - has BySectD patches)
      // 2. InputSection's patched data (from processRelocations)
      // 3. Original data from input file
      // BUG FIX: patchedCode must be checked FIRST because replaceImportCalls()
      // runs after processRelocations() and starts from hasPatchedData() if available,
      // so it already includes any processRelocations patches plus BySectD adjustments.

      auto patchedIt = patchedCode.find(isec);
      if (patchedIt != patchedCode.end()) {
        // Use patched code from replaceImportCalls (includes BySectD patches)
        const std::vector<uint8_t> &data = patchedIt->second;
        memcpy(buf, data.data(), data.size());
        buf += data.size();
      } else if (isec->hasPatchedData()) {
        // Use patched data from relocation processing
        ArrayRef<uint8_t> data = isec->getPatchedData();
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

  // TVect position is after import table AND 8-byte padding
  // Layout: [Import table][Padding (8 bytes)][Entry TVector]
  uint32_t importTableSize = totalImportedSymbolCount * 4;
  uint32_t paddingSize = 8;  // CodeWarrior adds 8 bytes padding before TVector
  tvectOffset = importTableSize + paddingSize;  // TVect comes after import table + padding

  if (config->verbose) {
    errorHandler().outs() << "Updated entry point TVect (CodeWarrior model):\n"
                         << "  TOC address: 0x" << utohexstr(tocAddress)
                         << " (r2 = data section start)\n"
                         << "  TVect offset: 0x" << utohexstr(tvectOffset)
                         << " (after " << importTableSize << " byte import table + "
                         << paddingSize << " byte padding)\n";
  }
}

// Collect functions whose addresses are taken across all input files.
// Only these functions need TVectors (plus the entry point).
// This prevents TOC overflow for large binaries like Rust with core/alloc.
void Writer::collectAddressTakenFunctions() {
  if (config->verbose) {
    errorHandler().outs() << "\nCollecting address-taken functions for sparse TVector generation...\n";
  }

  globalAddressTakenFunctionNames.clear();

  // Entry point always needs a TVector
  if (!config->entry.empty()) {
    Symbol *entrySym = symtab->find(config->entry);
    if (entrySym && entrySym->isDefined()) {
      globalAddressTakenFunctionNames.insert(entrySym->getName());
      if (config->verbose) {
        errorHandler().outs() << "  Entry point: " << config->entry << "\n";
      }
    }
  }

  // Aggregate address-taken functions from all ELF input files
  for (OutputSection *osec : outputSections) {
    for (InputSection *isec : osec->getInputSections()) {
      InputFile *file = isec->getFile();
      if (auto *elfFile = dyn_cast<ELFObjFile>(file)) {
        for (Symbol *sym : elfFile->getAddressTakenFunctions()) {
          // Store symbol name for name-based lookup
          // This handles both global and file-local symbols correctly
          if (sym->isDefined()) {
            globalAddressTakenFunctionNames.insert(sym->getName());
            if (config->verbose) {
              errorHandler().outs() << "  Address-taken: " << sym->getName() << "\n";
            }
          }
        }
      }
    }
  }

  // VTABLE FIX: Scan data section R_PPC_ADDR32 relocations to find vtable function pointers
  // These are function pointers stored in read-only data (vtables) that use section-relative
  // relocations (.text + offset) instead of direct function symbol references.
  // We need to identify which functions are referenced and add them to the TVector list.
  if (config->verbose) {
    errorHandler().outs() << "  Scanning data sections for vtable function pointers...\n";
  }

  // Build a map of code addresses to function symbols for lookup
  DenseMap<uint64_t, Defined*> codeAddressToSymbol;
  for (OutputSection *osec : outputSections) {
    if (osec->getKind() != PEF::kPEFCodeSection)
      continue;
    for (InputSection *isec : osec->getInputSections()) {
      if (!isec->isFromELF())
        continue;
      InputFile *file = isec->getFile();
      if (auto *elfFile = dyn_cast<ELFObjFile>(file)) {
        for (Symbol *sym : elfFile->getSymbols()) {
          if (!sym->isDefined())
            continue;
          auto *def = dyn_cast<Defined>(sym);
          if (!def || def->getSymbolClass() != PEF::kPEFCodeSymbol)
            continue;
          // Skip section symbols (like .text itself)
          if (sym->getName().starts_with("."))
            continue;
          codeAddressToSymbol[def->getVirtualAddress()] = def;
        }
      }
    }
  }

  // Find code section for VA calculations
  // Note: For PEF, code section VA is typically 0 (section-relative addressing)
  OutputSection *codeSection = nullptr;
  for (OutputSection *sec : outputSections) {
    if (sec->getKind() == PEF::kPEFCodeSection) {
      codeSection = sec;
      break;
    }
  }
  uint64_t codeSectionVA = codeSection ? codeSection->getVirtualAddress() : 0;

  // Now scan data sections for R_PPC_ADDR32 relocations targeting code
  for (OutputSection *osec : outputSections) {
    if (!isDataSection(osec->getKind()))
      continue;
    for (InputSection *isec : osec->getInputSections()) {
      if (!isec->isFromELF())
        continue;
      ArrayRef<InputSectionReloc> relocs = isec->getELFRelocations();
      for (const InputSectionReloc &reloc : relocs) {
        // Only process R_PPC_ADDR32 (type 1)
        if (reloc.type != 1)
          continue;
        Symbol *sym = reloc.symbol;
        if (!sym || !sym->isDefined())
          continue;
        auto *def = dyn_cast<Defined>(sym);
        if (!def || def->getSymbolClass() != PEF::kPEFCodeSymbol)
          continue;

        // Calculate target address
        // For section symbols (.text), getValue() returns 0, so we need to use
        // the input section's VA + addend to get the function's actual VA.
        // For named symbols, getVirtualAddress() gives the correct result directly.
        uint64_t targetAddr;
        if (sym->getName().starts_with(".")) {
          // Section symbol - compute VA from the specific input section + addend
          // The addend is the offset within that specific input section
          // We need to find the InputSection corresponding to this symbol's file + section index
          InputFile *symFile = def->getFile();
          int16_t symSecIdx = def->getSectionIndex();
          InputSection *targetIsec = nullptr;

          // Search for the InputSection matching this symbol's file and section index
          for (OutputSection *codeSec : outputSections) {
            if (codeSec->getKind() != PEF::kPEFCodeSection)
              continue;
            for (InputSection *codeIsec : codeSec->getInputSections()) {
              if (codeIsec->getFile() == symFile &&
                  static_cast<int16_t>(codeIsec->getIndex()) == symSecIdx) {
                targetIsec = codeIsec;
                break;
              }
            }
            if (targetIsec) break;
          }

          if (targetIsec) {
            targetAddr = targetIsec->getVirtualAddress() + reloc.addend;
            if (config->verbose) {
              errorHandler().outs() << "    Section symbol " << sym->getName()
                                   << " from " << symFile->getName()
                                   << " section " << symSecIdx
                                   << ": input VA 0x" << utohexstr(targetIsec->getVirtualAddress())
                                   << " + addend 0x" << utohexstr(reloc.addend)
                                   << " = target VA 0x" << utohexstr(targetAddr) << "\n";
            }
          } else {
            // Fallback: use output section VA (may be incorrect for merged sections)
            targetAddr = codeSectionVA + reloc.addend;
            if (config->verbose) {
              errorHandler().outs() << "    WARNING: Could not find input section for "
                                   << sym->getName() << ", using fallback VA 0x"
                                   << utohexstr(targetAddr) << "\n";
            }
          }
        } else {
          // Named function symbol - use its VA directly
          targetAddr = def->getVirtualAddress();
        }

        // Look up the function at this address
        auto it = codeAddressToSymbol.find(targetAddr);
        if (it != codeAddressToSymbol.end()) {
          Defined *targetFunc = it->second;
          globalAddressTakenFunctionNames.insert(targetFunc->getName());
        } else {
          // VTABLE FIX: No named symbol at this address - it's a local/anonymous function
          // Track the code address directly for TVector creation
          vtableCodeAddresses.insert(targetAddr);
        }
      }
    }
  }

  // FUNCTION POINTER FIX: Also scan CODE sections for HA/LO pairs that load
  // function addresses. These are used by core::fmt and other generic code
  // to store function pointers in Arguments structs.
  //
  // Pattern: R_PPC_ADDR16_HA (type 6) + R_PPC_ADDR16_LO (type 4) with same addend
  // targeting .text section symbol. This indicates loading a function address
  // into a register, which requires a TVector for CFM calling convention.
  if (config->verbose) {
    errorHandler().outs() << "  Scanning CODE sections for HA/LO function pointer loads...\n";
  }

  for (OutputSection *osec : outputSections) {
    if (osec->getKind() != PEF::kPEFCodeSection)
      continue;
    for (InputSection *isec : osec->getInputSections()) {
      if (!isec->isFromELF())
        continue;
      ArrayRef<InputSectionReloc> relocs = isec->getELFRelocations();
      for (size_t i = 0; i < relocs.size(); i++) {
        const InputSectionReloc &reloc = relocs[i];
        // Look for R_PPC_ADDR16_HA (type 6) targeting .text
        if (reloc.type != 6)  // R_PPC_ADDR16_HA
          continue;
        Symbol *sym = reloc.symbol;
        if (!sym)
          continue;
        // Only process references to .text section symbol
        if (!sym->getName().starts_with(".text"))
          continue;

        // Check if there's a matching R_PPC_ADDR16_LO with the same addend
        // (could be anywhere in the relocation list, not necessarily next)
        for (size_t j = 0; j < relocs.size(); j++) {
          if (i == j)
            continue;
          const InputSectionReloc &loReloc = relocs[j];
          if (loReloc.type == 4 &&  // R_PPC_ADDR16_LO
              loReloc.addend == reloc.addend &&
              loReloc.symbol == reloc.symbol) {
            // Found HA/LO pair - this loads a function address
            uint64_t targetAddr = reloc.addend;  // addend is offset into .text

            // Skip if this address is already tracked
            if (vtableCodeAddresses.count(targetAddr) == 0) {
              vtableCodeAddresses.insert(targetAddr);
              if (config->verbose) {
                errorHandler().outs() << "    Found HA/LO pair loading code address 0x"
                                     << utohexstr(targetAddr) << " (function pointer)\n";
              }
            }
            break;  // Found the matching LO, move to next HA
          }
        }
      }
    }
  }

  if (config->verbose) {
    errorHandler().outs() << "  Total named functions needing TVectors: "
                         << globalAddressTakenFunctionNames.size() << "\n";
    errorHandler().outs() << "  Total anonymous vtable functions needing TVectors: "
                         << vtableCodeAddresses.size() << "\n";
  }
}

// Create TVectors for address-taken function symbols
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

  // Collect ONLY address-taken code symbols (sparse TVector generation)
  // This prevents TOC overflow for large binaries like Rust with core/alloc
  // Note: codeFunctions is now a member variable so it can be used in assignFileOffsets()
  codeFunctions.clear();  // Clear any previous data
  for (size_t outSecIdx = 0; outSecIdx < outputSections.size(); ++outSecIdx) {
    OutputSection *osec = outputSections[outSecIdx];
    if (osec->getKind() != PEF::kPEFCodeSection)
      continue;

    for (InputSection *isec : osec->getInputSections()) {
      InputFile *file = isec->getFile();

      for (Symbol *sym : file->getSymbols()) {
        if (!sym->isDefined())
          continue;

        auto *def = cast<Defined>(sym);
        // Only create TVectors for code symbols that are in THIS code output section
        // AND whose address is taken (or is the entry point)
        // Note: After section merging, def->getSectionIndex() is the OUTPUT section index,
        // not the input section index.
        // Use name-based lookup to handle symbol identity mismatches
        if (def->getSymbolClass() == PEF::kPEFCodeSymbol &&
            def->getSectionIndex() == static_cast<int16_t>(outSecIdx) &&
            globalAddressTakenFunctionNames.count(sym->getName()) > 0) {
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
  // FIX: Create TVectors for ALL defined functions, not just entry point
  // This enables function pointers to work correctly (e.g., atexit handlers)
  // Layout: [Import table][Padding (8 bytes)][Function TVectors (8 bytes each)]
  uint32_t importTableSize = totalImportedSymbolCount * 4;
  uint32_t paddingSize = 8;  // CodeWarrior adds 8 bytes padding before TVector
  functionTVectorsOffset = importTableSize + paddingSize;  // After import table + padding
  functionTVectorsSize = codeFunctions.size() * 8;  // ALL functions get TVectors (8 bytes each)

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

  // VTABLE FIX: Create TVectors for anonymous vtable functions (functions without global symbols)
  // These were detected in collectAddressTakenFunctions() from R_PPC_ADDR32 relocations in data sections
  // They use section-relative addressing (.text + offset) and don't have named symbols.
  if (!vtableCodeAddresses.empty()) {
    if (config->verbose) {
      errorHandler().outs() << "  Creating TVectors for " << vtableCodeAddresses.size() << " anonymous vtable functions:\n";
    }

    for (uint64_t codeAddr : vtableCodeAddresses) {
      // Record mapping from code address to TVector offset
      vtableCodeAddressToTVector[codeAddr] = currentOffset;

      if (config->verbose) {
        errorHandler().outs() << "    Anonymous function at VA 0x" << utohexstr(codeAddr)
                             << ": TVector8 at offset 0x" << utohexstr(currentOffset) << "\n";
      }

      currentOffset += 8;
    }

    // Update the total TVector size to include anonymous function TVectors
    functionTVectorsSize = currentOffset - functionTVectorsOffset;
  }

  if (config->verbose) {
    errorHandler().outs() << "  TVector table: offset=0x" << utohexstr(functionTVectorsOffset)
                         << " size=" << functionTVectorsSize << " bytes"
                         << " (named: " << codeFunctions.size()
                         << ", anonymous: " << vtableCodeAddresses.size() << ")\n";
  }
}

// VTABLE FIX: Patch vtable function pointers to point to TVectors
// This must be called BEFORE assignFileOffsets() so the patched data is included
// in the pattern-encoded data section output.
void Writer::patchVTableEntries() {
  // Rust vtables contain function pointers as 4-byte code addresses.
  // Classic Mac OS requires function pointers to point to TVectors, not code.
  // For each R_PPC_ADDR32 relocation in data sections targeting a code symbol:
  // 1. Look up the function's TVector offset in functionTVectors or vtableCodeAddressToTVector
  // 2. Patch the data section with the TVector offset (relative to data section)
  // 3. Record the relocation for BySectD emission so CFM adds data section base at runtime

  if (config->verbose) {
    errorHandler().outs() << "\n=== VTABLE FIX: Patching vtable function pointers ===\n";
  }

  int vtableRelocsPatched = 0;

  // Find the data section
  OutputSection *dataSection = nullptr;
  for (OutputSection *osec : outputSections) {
    if (isDataSection(osec->getKind())) {
      dataSection = osec;
      break;
    }
  }

  if (!dataSection) {
    return;
  }


  for (InputSection *isec : dataSection->getInputSections()) {
    if (!isec->isFromELF())
      continue;

    ArrayRef<InputSectionReloc> relocs = isec->getELFRelocations();
    if (relocs.empty())
      continue;


    // Initialize patched data if not already done
    isec->initializePatchedData();
    std::vector<uint8_t> &data = isec->getMutablePatchedData();

    for (const InputSectionReloc &reloc : relocs) {
      // Only process R_PPC_ADDR32 (type 1)
      if (reloc.type != 1)
        continue;

      Symbol *sym = reloc.symbol;
      if (!sym || !sym->isDefined())
        continue;

      auto *def = dyn_cast<Defined>(sym);
      if (!def)
        continue;

      // Only process code symbols (function pointers in vtables)
      if (def->getSymbolClass() != PEF::kPEFCodeSymbol)
        continue;

      // Calculate target address using the same logic as collectAddressTakenFunctions()
      // For section symbols, we need to find the InputSection and use its VA
      uint64_t targetAddr;
      if (sym->getName().starts_with(".")) {
        // Section symbol - compute VA from the specific input section + addend
        InputFile *symFile = def->getFile();
        int16_t symSecIdx = def->getSectionIndex();
        InputSection *targetIsec = nullptr;

        // Search for the InputSection matching this symbol's file and section index
        for (OutputSection *codeSec : outputSections) {
          if (codeSec->getKind() != PEF::kPEFCodeSection)
            continue;
          for (InputSection *codeIsec : codeSec->getInputSections()) {
            if (codeIsec->getFile() == symFile &&
                static_cast<int16_t>(codeIsec->getIndex()) == symSecIdx) {
              targetIsec = codeIsec;
              break;
            }
          }
          if (targetIsec) break;
        }

        if (targetIsec) {
          targetAddr = targetIsec->getVirtualAddress() + reloc.addend;
        } else {
          // Fallback - should not happen if collectAddressTakenFunctions() worked
          targetAddr = def->getValue() + reloc.addend;
        }
      } else {
        // Named function symbol - use its VA directly
        targetAddr = def->getVirtualAddress();
      }

      // Search for TVector offset
      uint32_t tvectorOffset = 0;
      bool foundTVector = false;

      // First check named functions in functionTVectors
      for (const auto &entry : functionTVectors) {
        Defined *func = cast<Defined>(entry.first);
        if (func->getVirtualAddress() == targetAddr) {
          tvectorOffset = entry.second;
          foundTVector = true;
          break;
        }
      }

      // If not found, check anonymous vtable functions
      if (!foundTVector) {
        auto anonIt = vtableCodeAddressToTVector.find(targetAddr);
        if (anonIt != vtableCodeAddressToTVector.end()) {
          tvectorOffset = anonIt->second;
          foundTVector = true;
        }
      }

      if (!foundTVector) {
        if (config->verbose) {
          errorHandler().outs() << "  FAILED to find TVector for targetAddr 0x"
                               << utohexstr(targetAddr)
                               << " (sym: " << sym->getName()
                               << ", addend: 0x" << utohexstr(reloc.addend) << ")\n";
          errorHandler().outs() << "    functionTVectors has " << functionTVectors.size() << " entries\n";
          errorHandler().outs() << "    vtableCodeAddressToTVector has "
                               << vtableCodeAddressToTVector.size() << " entries:\n";
          for (const auto &entry : vtableCodeAddressToTVector) {
            errorHandler().outs() << "      0x" << utohexstr(entry.first)
                                 << " -> offset 0x" << utohexstr(entry.second) << "\n";
          }
        }
        continue;
      }

      // Patch the data section with the TVector offset
      if (reloc.offset + 4 <= data.size()) {
        // Read the old value for debugging
        uint32_t oldValue = (static_cast<uint32_t>(data[reloc.offset + 0]) << 24) |
                           (static_cast<uint32_t>(data[reloc.offset + 1]) << 16) |
                           (static_cast<uint32_t>(data[reloc.offset + 2]) << 8) |
                           static_cast<uint32_t>(data[reloc.offset + 3]);

        // Write the TVector offset as big-endian 32-bit value
        data[reloc.offset + 0] = (tvectorOffset >> 24) & 0xFF;
        data[reloc.offset + 1] = (tvectorOffset >> 16) & 0xFF;
        data[reloc.offset + 2] = (tvectorOffset >> 8) & 0xFF;
        data[reloc.offset + 3] = tvectorOffset & 0xFF;

        // Record this relocation for BySectD emission in RelocWriter
        // Store the InputSection and reloc offset - the final output offset will be
        // computed later by RelocWriter using inputSectionOutputOffsets map
        // (after assignFileOffsets() reorders sections for TOC overflow prevention)
        vtableRelocations.push_back({isec, static_cast<uint32_t>(reloc.offset)});

        vtableRelocsPatched++;

        if (config->verbose) {
          errorHandler().outs() << "  Patched vtable entry at offset 0x" << utohexstr(reloc.offset)
                               << " in section " << isec->getName()
                               << ": 0x" << utohexstr(oldValue) << " -> 0x" << utohexstr(tvectorOffset) << "\n";
        }
      } else {
        error("vtable relocation offset out of bounds: " + Twine(reloc.offset));
      }
    }
  }

  if (config->verbose) {
    errorHandler().outs() << "=== VTABLE FIX: Patched " << vtableRelocsPatched << " vtable function pointer(s) ===\n";
  }

  // DATA-TO-DATA RELOCATION FIX: Handle pointers to .rodata strings
  // core::fmt::Arguments contains &str slices pointing to .rodata strings.
  // These are R_PPC_ADDR32 relocations in data section targeting data symbols.
  //
  // We CANNOT patch the values here because:
  // - Sections get reordered during assignFileOffsets() for TOC overflow prevention
  // - VAs at this point are stale and will be updated during the final pass
  //
  // Instead, we:
  // 1. Record the relocation info (source section, offset, target section, addend)
  // 2. The actual patching happens in assignFileOffsets() when we know final positions
  // 3. Emit BySectD relocations so CFM adds data section base at runtime
  int dataToDataRelocsRecorded = 0;

  for (InputSection *isec : dataSection->getInputSections()) {
    if (!isec->isFromELF())
      continue;

    ArrayRef<InputSectionReloc> relocs = isec->getELFRelocations();
    if (relocs.empty())
      continue;

    for (const InputSectionReloc &reloc : relocs) {
      // Only process R_PPC_ADDR32 (type 1)
      if (reloc.type != 1)
        continue;

      Symbol *sym = reloc.symbol;
      if (!sym || !sym->isDefined())
        continue;

      auto *def = dyn_cast<Defined>(sym);
      if (!def)
        continue;

      // Only process DATA symbols (skip code symbols - handled above)
      if (def->getSymbolClass() == PEF::kPEFCodeSymbol)
        continue;

      // Find the target InputSection and compute the final addend
      InputSection *targetIsec = nullptr;
      uint64_t finalAddend = reloc.addend;  // Start with reloc addend

      InputFile *symFile = def->getFile();
      int16_t symSecIdx = def->getSectionIndex();

      // For section symbols (like .rodata), the section index is still the INPUT section index
      // For named symbols, after resolution, getSectionIndex() returns the OUTPUT section index
      // So we need different strategies for each case
      bool isSectionSymbol = sym->getName().starts_with(".");

      if (isSectionSymbol) {
        // Section symbol: Match by file + input section index
        for (InputSection *dataIsec : dataSection->getInputSections()) {
          if (dataIsec->getFile() == symFile &&
              static_cast<int16_t>(dataIsec->getIndex()) == symSecIdx) {
            targetIsec = dataIsec;
            break;
          }
        }
        // Fallback: try matching by file name (for archive members)
        if (!targetIsec && symFile) {
          StringRef symFileName = symFile->getName();
          for (InputSection *dataIsec : dataSection->getInputSections()) {
            if (dataIsec->getFile() &&
                dataIsec->getFile()->getName() == symFileName &&
                static_cast<int16_t>(dataIsec->getIndex()) == symSecIdx) {
              targetIsec = dataIsec;
              break;
            }
          }
        }
      } else {
        // Named symbol: Use the symbol's VA to find which InputSection contains it
        // After resolution, def->getValue() is the offset within the OUTPUT section
        // and def->getVirtualAddress() is the final VA in the merged output
        uint64_t symVA = def->getVirtualAddress();

        // Find the InputSection that contains this VA
        for (InputSection *dataIsec : dataSection->getInputSections()) {
          uint64_t isecVA = dataIsec->getVirtualAddress();
          uint64_t isecSize = dataIsec->getSize();
          if (symVA >= isecVA && symVA < isecVA + isecSize) {
            targetIsec = dataIsec;
            // The finalAddend should be the offset from the InputSection's start
            // symVA = isecVA + offset, so offset = symVA - isecVA
            finalAddend = (symVA - isecVA) + reloc.addend;
            if (config->verbose) {
              errorHandler().outs() << "    Found named symbol " << sym->getName()
                                   << " at VA 0x" << utohexstr(symVA)
                                   << " in InputSection " << dataIsec->getName()
                                   << " (VA 0x" << utohexstr(isecVA) << " size 0x"
                                   << utohexstr(isecSize) << ")\n"
                                   << "    Computed finalAddend = 0x" << utohexstr(finalAddend) << "\n";
            }
            break;
          }
        }
      }

      if (!targetIsec) {
        if (config->verbose) {
          errorHandler().outs() << "    WARNING: Could not find target input section for "
                               << sym->getName() << " from " << symFile->getName()
                               << " section " << symSecIdx
                               << " (isSectionSymbol=" << isSectionSymbol << ")\n";
          if (!isSectionSymbol) {
            errorHandler().outs() << "    Symbol VA: 0x" << utohexstr(def->getVirtualAddress())
                                 << " value: 0x" << utohexstr(def->getValue()) << "\n";
          }
          // Debug: print available sections from this file
          errorHandler().outs() << "    Available data sections:\n";
          for (InputSection *dataIsec : dataSection->getInputSections()) {
            errorHandler().outs() << "      " << dataIsec->getName()
                                 << " from " << (dataIsec->getFile() ? dataIsec->getFile()->getName() : "unknown")
                                 << " VA 0x" << utohexstr(dataIsec->getVirtualAddress())
                                 << " size 0x" << utohexstr(dataIsec->getSize()) << "\n";
          }
        }
        continue;
      }

      // For section symbols, add the addend (which is the offset within that section)
      // For named symbols, we already computed finalAddend above

      // Record the relocation for later patching during assignFileOffsets()
      pendingDataToDataRelocs.push_back({
        isec,                               // source section
        static_cast<uint32_t>(reloc.offset), // offset within source section
        targetIsec,                          // target section
        finalAddend                          // addend (includes symbol value for named symbols)
      });

      // Record for BySectD emission (using source section and offset)
      vtableRelocations.push_back({isec, static_cast<uint32_t>(reloc.offset)});
      dataToDataRelocsRecorded++;

      if (config->verbose) {
        errorHandler().outs() << "  Recorded data-to-data reloc at offset 0x" << utohexstr(reloc.offset)
                             << " in section " << isec->getName()
                             << " -> " << sym->getName() << "+0x" << utohexstr(reloc.addend) << "\n";
      }
    }
  }

  if (config->verbose) {
    errorHandler().outs() << "=== DATA-TO-DATA FIX: Recorded " << dataToDataRelocsRecorded
                         << " relocation(s) for deferred patching ===\n";
  }
}

// Generate M68k CFM-68K import stubs (16 bytes each)
// These stubs load the TVect from the import table (A5-relative),
// save the caller's A5, load the target function and A5, then jump.
void Writer::generateImportStubsM68k() {
  // Generate one stub per imported symbol
  uint32_t stubIndex = 0;
  for (const auto &lib : importedLibraries) {
    for (ImportedSymbol *sym : lib.symbols) {
      uint32_t stubOffset = importStubs.size();
      stubOffsets[sym] = stubOffset;

      // A5-relative offset to import table slot
      // Import table is at the start of the data section (A5 base)
      int32_t importSlotOffset = stubIndex * 4;

      // CFM-68K import stub (16 bytes, 6 instructions):
      //
      // move.l  offset(a5), a1    ; Load TVect pointer from import table (4 bytes)
      // move.l  a5, -(sp)         ; Save caller's A5 on stack (2 bytes)
      // move.l  (a1)+, a0         ; Load function address from TVect[0] (2 bytes)
      // move.l  (a1), a5          ; Load target A5 from TVect[1] (2 bytes)
      // jmp     (a0)              ; Jump to function (2 bytes)
      // nop; nop                  ; Padding to 16 bytes (4 bytes)

      // 1. move.l offset(a5), a1  - Opcode: 0x226D + 16-bit offset
      //    Format: 0010 001 001 101 101 = 0x226D (move.l d(An), An)
      uint16_t moveA1 = 0x226D;
      importStubs.push_back((moveA1 >> 8) & 0xFF);
      importStubs.push_back(moveA1 & 0xFF);
      importStubs.push_back((importSlotOffset >> 8) & 0xFF);
      importStubs.push_back(importSlotOffset & 0xFF);

      // 2. move.l a5, -(sp)  - Opcode: 0x2F0D
      //    Format: 0010 111 100 001 101 = 0x2F0D (move.l An, -(A7))
      uint16_t moveA5ToStack = 0x2F0D;
      importStubs.push_back((moveA5ToStack >> 8) & 0xFF);
      importStubs.push_back(moveA5ToStack & 0xFF);

      // 3. move.l (a1)+, a0  - Opcode: 0x2059
      //    Format: 0010 000 001 011 001 = 0x2059 (move.l (An)+, An)
      uint16_t moveA0FromA1 = 0x2059;
      importStubs.push_back((moveA0FromA1 >> 8) & 0xFF);
      importStubs.push_back(moveA0FromA1 & 0xFF);

      // 4. move.l (a1), a5  - Opcode: 0x2A51
      //    Format: 0010 101 001 010 001 = 0x2A51 (move.l (An), An)
      uint16_t moveA5FromA1 = 0x2A51;
      importStubs.push_back((moveA5FromA1 >> 8) & 0xFF);
      importStubs.push_back(moveA5FromA1 & 0xFF);

      // 5. jmp (a0)  - Opcode: 0x4ED0
      //    Format: 0100 1110 1101 0000 = 0x4ED0 (jmp (An))
      uint16_t jmpA0 = 0x4ED0;
      importStubs.push_back((jmpA0 >> 8) & 0xFF);
      importStubs.push_back(jmpA0 & 0xFF);

      // 6. nop; nop  - Padding to 16 bytes (0x4E71 x 2)
      uint16_t nop = 0x4E71;
      importStubs.push_back((nop >> 8) & 0xFF);
      importStubs.push_back(nop & 0xFF);
      importStubs.push_back((nop >> 8) & 0xFF);
      importStubs.push_back(nop & 0xFF);

      if (config->verbose) {
        errorHandler().outs() << "  M68k stub for " << sym->getName()
                             << " at offset 0x" << utohexstr(stubOffset)
                             << " (A5 offset: " << importSlotOffset << ")\n";
      }

      stubIndex++;
    }
  }
}

// Generate PowerPC import stubs (24 bytes each)
void Writer::generateImportStubsPPC() {
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
        errorHandler().outs() << "  PPC stub for " << sym->getName()
                             << " at offset 0x" << utohexstr(stubOffset)
                             << " (TOC offset: " << offsetFromTOC << ")\n";
      }

      stubIndex++;
    }
  }
}

// BUG FIX #35: Generate import stubs in code section
// PowerPC: 24 bytes each, M68k: 16 bytes each
void Writer::generateImportStubs() {
  if (importedLibraries.empty()) {
    return;
  }

  if (config->verbose) {
    const char *archName = (config->architecture == PEFArch::M68k) ? "M68k" : "PowerPC";
    errorHandler().outs() << "\nGenerating " << archName << " import stubs in code section...\n";
  }

  // Find data section to calculate base offsets
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

  // Generate architecture-specific stubs
  if (config->architecture == PEFArch::M68k) {
    generateImportStubsM68k();
  } else {
    generateImportStubsPPC();
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
      // BUG FIX: Clear patchedPositions for each input section
      // relocPos is relative to the current input section, so positions from previous sections
      // should not affect this one (position 0x22 in section A != position 0x22 in section B)
      patchedPositions.clear();

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

      // BUG FIX #39: Use the MEMBER variable patchedPositions, not a local variable!
      // The member variable is passed to RelocWriter to skip emitting BySectD relocations
      // for positions that have already been patched by the linker.
      // Previously, a local variable shadowed the member, causing patched positions to be lost.

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
          // Note: isec->getVirtualAddress() already includes output section base
          uint32_t codeVA = isec->getVirtualAddress() + relocPos;

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
            // If we read opcode 0, check if we're at +2 offset into a lis or addi instruction
            if (instrOpcode == 0 && relocPos >= 2) {
              // Try reading 2 bytes earlier to check for lis or addi
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
              } else if (prevOpcode == 14) {  // Found addi instruction
                // Adjust relocPos to instruction start
                relocPos -= 2;
                instruction = prevInstr;
                instrOpcode = 14;

                if (config->verbose) {
                  errorHandler().outs() << "      Adjusted relocPos -2 bytes (addi immediate field → instruction start)\n";
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
            } else if (instrOpcode == 14) {
              // This is addi - check if it's TOC-relative (addi rD, r2, offset)
              uint8_t sourceReg = (instruction >> 16) & 0x1F;  // rA field

              if (sourceReg == 2) {
                // TOC-relative data reference: addi rD, r2, offset
                // This is used for accessing global data via the TOC
                // We need to patch the offset to point to the symbol's location in the data section

                // For data symbols, calculate TOC offset (offset within data section)
                // NOTE: Symbol class check removed - TOC-relative addressing works for any symbol
                // The original check for kPEFDataSymbol was too restrictive and caused qd to be skipped
                if (auto *defined = dyn_cast<Defined>(sym)) {
                  // Get data section to calculate offset
                  OutputSection *dataSection = nullptr;
                  for (OutputSection *osec : outputSections) {
                    if (isDataSection(osec->getKind())) {
                      dataSection = osec;
                      break;
                    }
                  }

                  if (dataSection) {
                    // Calculate TOC offset: symbol VA - data section base
                    uint64_t targetVA = defined->getVirtualAddress();
                    uint64_t dataBase = dataSection->getVirtualAddress();
                    uint32_t tocOffset = static_cast<uint32_t>(targetVA - dataBase);

                    // Patch the immediate field (lower 16 bits)
                    uint16_t newImm = tocOffset & 0xFFFF;
                    uint32_t newInstr = (instruction & 0xFFFF0000) | newImm;

                    code[relocPos] = (newInstr >> 24) & 0xFF;
                    code[relocPos + 1] = (newInstr >> 16) & 0xFF;
                    code[relocPos + 2] = (newInstr >> 8) & 0xFF;
                    code[relocPos + 3] = newInstr & 0xFF;

                    hasPatches = true;
                    patchedPositions.insert(relocPos);

                    if (config->verbose) {
                      errorHandler().outs() << "      Patched TOC-relative addi for symbol '"
                                           << sym->getName() << "' at 0x" << utohexstr(relocPos)
                                           << ": offset 0 -> 0x" << utohexstr(tocOffset)
                                           << " (VA=0x" << utohexstr(targetVA) << ")\n";
                    }
                  } else {
                    error("no data section found for TOC-relative addi");
                  }
                } else if (config->verbose) {
                  errorHandler().outs() << "  WARNING: TOC-relative addi for non-defined symbol "
                                       << sym->getName() << "\n";
                }
              } else if (config->verbose) {
                errorHandler().outs() << "  WARNING: addi instruction but not TOC-relative (rA="
                                     << (unsigned)sourceReg << ") for " << sym->getName() << "\n";
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
                  // Final layout: [Import table][Padding][Entry TVector][ObjFile data sections...]

                  // Get the offset of this object file's data section in the merged data
                  // This already includes the prepend (import table + padding + entry TVector)
                  // IMPORTANT: BySectD relocations in code reference data section addresses,
                  // so we need the data section offset, not the code section offset!
                  InputFile *objFile = isec->getFile();
                  uint64_t inputSectionOffset = 0;
                  if (objFile) {
                    auto it = objFileDataOffsets.find(objFile);
                    if (it != objFileDataOffsets.end()) {
                      inputSectionOffset = it->second;
                    } else {
                      // This object file has no data section, warn
                      if (config->verbose) {
                        errorHandler().outs() << "      WARNING: BySectD relocation in " << objFile->getName()
                                             << " but no data section found for this file\n";
                      }
                    }
                  }

                  uint32_t adjustedOffset = inputSectionOffset + currentOffset;

                  if (config->verbose) {
                    errorHandler().outs() << "      BySectD: inputSectionOffset=0x" << utohexstr(inputSectionOffset)
                                         << " currentOffset=0x" << utohexstr(currentOffset)
                                         << " adjustedOffset=0x" << utohexstr(adjustedOffset)
                                         << " for " << (objFile ? objFile->getName() : "unknown") << "\n";
                  }

                  // BUG FIX #40: Pass inputSectionOffset (the BASE), not adjustedOffset
                  // patchLisAddiPair reads currentOffset from instruction and adds to targetAddr
                  // If we pass adjustedOffset (= base + offset), it would double-add the offset!
                  //
                  // BUG FIX #40b: Do NOT add lis/addi positions to patchedPositions!
                  // lis/addi pairs use absolute addressing and NEED the BySectD relocation
                  // for CFM to add the runtime data_base. Only TOC-relative instructions
                  // (addi r3, r2, offset) should skip BySectD since r2 provides the base.
                  if (patchLisAddiPair(code, currentPos, inputSectionOffset, "data section")) {
                    hasPatches = true;
                    // NOTE: Do NOT add to patchedPositions - lis/addi needs BySectD relocation!

                    if (config->verbose) {
                      errorHandler().outs() << "      Patched BySectD lis/addi pair at 0x"
                                           << utohexstr(currentPos) << " with base offset 0x"
                                           << utohexstr(inputSectionOffset)
                                           << " (instruction offset=" << currentOffset << ")\n";
                    }
                  }
                }
              } else if (instrOpcode == 14 || instrOpcode == 36 || instrOpcode == 32) {
                // addi (14), stw (36), or lwz (32) - patch the immediate offset
                // Extract current offset (symbol's offset in OBJECT FILE data section)
                int16_t currentOffset = (int16_t)(instruction & 0xFFFF);

                // BUG FIX: Calculate offset in FINAL data section
                // Final layout: [Import table][Padding][Entry TVector][ObjFile data sections...]

                // Get the offset of this object file's data section in the merged data
                // This already includes the prepend (import table + padding + entry TVector)
                InputFile *objFile = isec->getFile();
                uint64_t inputSectionOffset = 0;
                if (objFile) {
                  auto it = objFileDataOffsets.find(objFile);
                  if (it != objFileDataOffsets.end()) {
                    inputSectionOffset = it->second;
                  }
                }

                uint32_t adjustedOffset = inputSectionOffset + currentOffset;

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
                                       << utohexstr(newOffset)
                                       << " (inputSecOffset=" << inputSectionOffset
                                       << " + mergeOffset=0x" << utohexstr((uint16_t)currentOffset) << ")\n";
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

                // This immediate is a CODE section offset relative to the object file's
                // code section. We need to find which function has this offset.
                // BUG FIX #41: The immediate is relative to the input section's base.
                // After linking, func->getValue() contains the FINAL virtual address.
                // To find the right function, calculate: isecBase + immediate
                InputFile *relocFile = isec->getFile();
                uint64_t isecBase = isec->getVirtualAddress();
                uint64_t targetVA = isecBase + immediate;  // Final VA of referenced function

                Defined *targetFunc = nullptr;
                for (const auto &entry : functionTVectors) {
                  Defined *func = cast<Defined>(entry.first);
                  // Match the final virtual address AND the source file
                  if (func->getValue() == targetVA && func->getFile() == relocFile) {
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

void Writer::processELFRelocations() {
  // Process ELF relocations for ELF input files
  //
  // PowerPC relocation types:
  // - R_PPC_REL24 (type 10): PC-relative branch (bl/b instructions)
  // - R_PPC_ADDR16 (type 3): 16-bit absolute/TOC-relative address
  // - R_PPC_ADDR16_LO (type 4): Low 16 bits for 32-bit addressing (Large code model)
  // - R_PPC_ADDR16_HA (type 6): High Adjusted 16 bits for 32-bit addressing
  //
  // M68k relocation types:
  // - R_68K_32 (type 1): Direct 32-bit absolute
  // - R_68K_PC32 (type 4): PC-relative 32-bit (BSR.L, etc.)
  // - R_68K_PC16 (type 5): PC-relative 16-bit (BSR.W, BRA.W, etc.)

  if (config->verbose) {
    const char *archName = (config->architecture == PEFArch::M68k) ? "M68k" : "PowerPC";
    errorHandler().outs() << "\nProcessing " << archName << " ELF relocations...\n";
  }

  // Find code section virtual address
  uint64_t codeSectionVA = 0;
  OutputSection *codeSection = nullptr;
  for (OutputSection *osec : outputSections) {
    if (osec->getKind() == PEF::kPEFCodeSection) {
      codeSection = osec;
      codeSectionVA = osec->getVirtualAddress();
      break;
    }
  }

  // Find data section virtual address (for TOC-relative addressing)
  uint64_t dataSectionVA = 0;
  for (OutputSection *osec : outputSections) {
    if (isDataSection(osec->getKind())) {
      dataSectionVA = osec->getVirtualAddress();
      break;
    }
  }

  if (!codeSection) {
    return;  // No code section
  }

  int relocsProcessed = 0;

  for (OutputSection *osec : outputSections) {
    if (osec->getKind() != PEF::kPEFCodeSection)
      continue;

    for (InputSection *isec : osec->getInputSections()) {
      if (config->verbose) {
        errorHandler().outs() << "  Checking section " << isec->getName()
                             << " isFromELF=" << isec->isFromELF() << "\n";
      }
      if (!isec->isFromELF())
        continue;

      ArrayRef<InputSectionReloc> relocs = isec->getELFRelocations();
      if (config->verbose) {
        errorHandler().outs() << "  Section " << isec->getName()
                             << " has " << relocs.size() << " ELF relocations\n";
      }
      if (relocs.empty())
        continue;

      // Initialize patched data if not already done
      isec->initializePatchedData();
      std::vector<uint8_t> &code = isec->getMutablePatchedData();

      uint64_t isecVA = isec->getVirtualAddress();

      for (const InputSectionReloc &reloc : relocs) {
        Symbol *sym = reloc.symbol;
        if (!sym) {
          // Skip relocations without symbols (e.g., section symbols that weren't found)
          continue;
        }

        // Re-lookup symbol by name since it may have been resolved to an import
        // after the ELF file was parsed
        Symbol *resolvedSym = symtab->find(sym->getName());
        if (!resolvedSym) {
          // Try to use original symbol if it's defined
          if (sym->isDefined()) {
            resolvedSym = sym;
          } else {
            if (config->verbose) {
              errorHandler().outs() << "  Warning: symbol not found: " << sym->getName() << "\n";
            }
            continue;
          }
        }
        sym = resolvedSym;

        // Architecture-specific relocation handling
        if (config->architecture == PEFArch::M68k) {
          // ===== M68k Relocation Handling =====

          // Handle R_68K_PC32 (type 4) - PC-relative 32-bit
          // Used for BSR.L (branch to subroutine, long form)
          if (reloc.type == 4) {
            if (reloc.offset + 4 > code.size()) {
              error("relocation offset out of bounds");
              continue;
            }

            // Calculate source address (PC points to start of displacement)
            // For M68k BSR.L, the displacement is relative to the PC which points
            // to the instruction AFTER the opcode word (i.e., at the displacement itself)
            uint64_t sourceAddr = isecVA + reloc.offset;

            // Calculate target address
            uint64_t targetAddr = 0;
            bool isImport = false;

            if (sym->isDefined()) {
              Defined *def = cast<Defined>(sym);
              targetAddr = def->getVirtualAddress();
            } else if (isa<ImportedSymbol>(sym)) {
              auto it = stubOffsets.find(sym);
              if (it == stubOffsets.end()) {
                error("no stub for imported symbol: " + sym->getName());
                continue;
              }
              uint32_t stubRegionStart = codeSection->getOriginalSize();
              targetAddr = codeSectionVA + stubRegionStart + it->second;
              isImport = true;
            } else {
              error("unresolved symbol for relocation: " + sym->getName());
              continue;
            }

            // Calculate PC-relative displacement
            int64_t displacement = static_cast<int64_t>(targetAddr) - static_cast<int64_t>(sourceAddr);

            // Write 32-bit displacement (big-endian)
            code[reloc.offset + 0] = (displacement >> 24) & 0xFF;
            code[reloc.offset + 1] = (displacement >> 16) & 0xFF;
            code[reloc.offset + 2] = (displacement >> 8) & 0xFF;
            code[reloc.offset + 3] = displacement & 0xFF;

            relocsProcessed++;

            if (config->verbose) {
              errorHandler().outs() << "  R_68K_PC32 at 0x" << utohexstr(reloc.offset)
                                   << " -> " << sym->getName()
                                   << (isImport ? " (stub)" : " (internal)")
                                   << " disp=" << displacement << "\n";
            }
          }
          // Handle R_68K_PC16 (type 5) - PC-relative 16-bit
          // Used for BSR.W (branch to subroutine, word form), BRA.W, etc.
          else if (reloc.type == 5) {
            if (reloc.offset + 2 > code.size()) {
              error("relocation offset out of bounds");
              continue;
            }

            uint64_t sourceAddr = isecVA + reloc.offset;

            uint64_t targetAddr = 0;
            bool isImport = false;

            if (sym->isDefined()) {
              Defined *def = cast<Defined>(sym);
              targetAddr = def->getVirtualAddress();
            } else if (isa<ImportedSymbol>(sym)) {
              auto it = stubOffsets.find(sym);
              if (it == stubOffsets.end()) {
                error("no stub for imported symbol: " + sym->getName());
                continue;
              }
              uint32_t stubRegionStart = codeSection->getOriginalSize();
              targetAddr = codeSectionVA + stubRegionStart + it->second;
              isImport = true;
            } else {
              error("unresolved symbol for relocation: " + sym->getName());
              continue;
            }

            int64_t displacement = static_cast<int64_t>(targetAddr) - static_cast<int64_t>(sourceAddr);

            // Check displacement fits in 16 bits
            if (displacement < -32768 || displacement > 32767) {
              error("PC16 displacement out of range for " + sym->getName() +
                    ": " + Twine(displacement));
              continue;
            }

            // Write 16-bit displacement (big-endian)
            code[reloc.offset + 0] = (displacement >> 8) & 0xFF;
            code[reloc.offset + 1] = displacement & 0xFF;

            relocsProcessed++;

            if (config->verbose) {
              errorHandler().outs() << "  R_68K_PC16 at 0x" << utohexstr(reloc.offset)
                                   << " -> " << sym->getName()
                                   << (isImport ? " (stub)" : " (internal)")
                                   << " disp=" << displacement << "\n";
            }
          }
          // Handle R_68K_32 (type 1) - Direct 32-bit absolute
          // Used for absolute addresses (data pointers, vtables, etc.)
          else if (reloc.type == 1) {
            if (reloc.offset + 4 > code.size()) {
              error("relocation offset out of bounds");
              continue;
            }

            uint64_t targetAddr = 0;

            if (sym->isDefined()) {
              Defined *def = cast<Defined>(sym);
              targetAddr = def->getVirtualAddress() + reloc.addend;
            } else if (isa<ImportedSymbol>(sym)) {
              // For imported symbols in data, we patch with import table slot address
              // The CFM loader will fill in the actual address at load time
              // For now, we'll leave this as 0 and let the relocation opcodes handle it
              targetAddr = reloc.addend;  // Will be patched by CFM loader
            } else {
              error("unresolved symbol for R_68K_32: " + sym->getName());
              continue;
            }

            // Write 32-bit address (big-endian)
            code[reloc.offset + 0] = (targetAddr >> 24) & 0xFF;
            code[reloc.offset + 1] = (targetAddr >> 16) & 0xFF;
            code[reloc.offset + 2] = (targetAddr >> 8) & 0xFF;
            code[reloc.offset + 3] = targetAddr & 0xFF;

            relocsProcessed++;

            if (config->verbose) {
              errorHandler().outs() << "  R_68K_32 at 0x" << utohexstr(reloc.offset)
                                   << " -> " << sym->getName()
                                   << " addr=0x" << utohexstr(targetAddr) << "\n";
            }
          }
          continue;  // Skip PowerPC relocation handling for M68k
        }

        // ===== PowerPC Relocation Handling =====

        // Handle R_PPC_REL24 (type 10) - PC-relative branch
        if (reloc.type == 10) {
          if (reloc.offset + 4 > code.size()) {
            error("relocation offset out of bounds");
            continue;
          }

          // Calculate source address (where the branch instruction is)
          uint64_t sourceAddr = isecVA + reloc.offset;

          // Calculate target address
          uint64_t targetAddr = 0;
          bool isImport = false;

          if (sym->isDefined()) {
            // Internal function call - use symbol's virtual address
            Defined *def = cast<Defined>(sym);
            targetAddr = def->getVirtualAddress();
          } else if (isa<ImportedSymbol>(sym)) {
            // Imported function - use import stub address
            auto it = stubOffsets.find(sym);
            if (it == stubOffsets.end()) {
              error("no stub for imported symbol: " + sym->getName());
              continue;
            }
            // Stub offset is within import stubs buffer, which is APPENDED after user code
            // Final stub address = codeSectionVA + originalCodeSize + stubOffset
            uint32_t stubRegionStart = codeSection->getOriginalSize();
            targetAddr = codeSectionVA + stubRegionStart + it->second;
            isImport = true;
          } else {
            error("unresolved symbol for relocation: " + sym->getName());
            continue;
          }

          // Calculate PC-relative offset
          // Branch instruction format: opcode(6) | LI(24) | AA(1) | LK(1)
          int64_t displacement = static_cast<int64_t>(targetAddr) - static_cast<int64_t>(sourceAddr);

          // Check displacement fits in 24 bits (26 bits effective with shift)
          if (displacement < -0x2000000 || displacement > 0x1FFFFFC) {
            error("branch displacement out of range: " + Twine(displacement));
            continue;
          }

          // Read existing instruction to preserve opcode and LK bit
          uint32_t instr = (code[reloc.offset] << 24) |
                           (code[reloc.offset + 1] << 16) |
                           (code[reloc.offset + 2] << 8) |
                           code[reloc.offset + 3];

          // Preserve opcode (bits 0-5) and LK bit (bit 31), update LI field (bits 6-29)
          uint32_t opcode = instr & 0xFC000000;  // Top 6 bits
          uint32_t lk = instr & 0x1;             // LK bit
          uint32_t li = (displacement & 0x03FFFFFC);  // 24-bit displacement, low 2 bits 0
          uint32_t newInstr = opcode | li | lk;

          // Write patched instruction
          code[reloc.offset + 0] = (newInstr >> 24) & 0xFF;
          code[reloc.offset + 1] = (newInstr >> 16) & 0xFF;
          code[reloc.offset + 2] = (newInstr >> 8) & 0xFF;
          code[reloc.offset + 3] = newInstr & 0xFF;

          relocsProcessed++;

          if (config->verbose) {
            errorHandler().outs() << "  R_PPC_REL24 at 0x" << utohexstr(reloc.offset)
                                 << " -> " << sym->getName()
                                 << (isImport ? " (stub)" : " (internal)")
                                 << " disp=" << displacement << "\n";
          }
        }
        // Handle R_PPC_ADDR16 (type 3) - 16-bit TOC-relative address
        // This patches the low 16 bits of an instruction with a TOC offset
        // Used for: addi rD, r2, offset (accessing global data via TOC)
        // For function pointers: redirects to TVector address in data section
        else if (reloc.type == 3) {
          if (reloc.offset + 2 > code.size()) {
            error("relocation offset out of bounds");
            continue;
          }

          // Calculate the symbol's offset relative to TOC (r2)
          // In PEF, r2 points to the start of the data section
          int32_t tocOffset = 0;
          uint64_t symbolVA = 0;

          if (sym->isDefined()) {
            Defined *def = cast<Defined>(sym);

            // Check if this is a code symbol (function) being used as a function pointer
            // In CFM/PEF, function pointers are TVector addresses, not code addresses
            if (def->getSymbolClass() == PEF::kPEFCodeSymbol) {
              // Look up the function's TVector in our table
              auto tvIt = functionTVectors.find(def);
              if (tvIt != functionTVectors.end()) {
                // Use the TVector offset directly (it's already relative to data section)
                tocOffset = static_cast<int32_t>(tvIt->second);
                if (config->verbose) {
                  errorHandler().outs() << "  R_PPC_ADDR16 at 0x" << utohexstr(reloc.offset)
                                       << " -> " << sym->getName()
                                       << " (function pointer -> TVector at offset "
                                       << tocOffset << ")\n";
                }
                // Add the addend from the relocation
                tocOffset += reloc.addend;

                // Check if offset fits in signed 16 bits
                if (tocOffset < -32768 || tocOffset > 32767) {
                  error("TOC offset overflow for function TVector '" + sym->getName() +
                        "': offset " + Twine(tocOffset) + " exceeds 16-bit signed range [-32768, 32767]. "
                        "The data section has grown too large. Consider:\n"
                        "  - Reducing the number of address-taken functions\n"
                        "  - Moving large static data to a separate compilation unit\n"
                        "  - Building with less debug info or fewer libraries");
                  continue;
                }

                // Patch the low 16 bits of the instruction
                code[reloc.offset + 0] = (tocOffset >> 8) & 0xFF;
                code[reloc.offset + 1] = tocOffset & 0xFF;
                relocsProcessed++;
                continue;  // Skip the rest of R_PPC_ADDR16 handling
              }
              // Code symbol has address taken but no TVector was created
              // This indicates a bug in address-taken detection during relocation parsing
              error("code symbol '" + sym->getName() +
                    "' referenced via R_PPC_ADDR16 but has no TVector. "
                    "This is likely a bug in address-taken detection. "
                    "Check if the symbol is a section symbol or file-local symbol "
                    "that wasn't properly tracked (relocation type " +
                    Twine(reloc.type) + ")");
              continue;
            }

            symbolVA = def->getVirtualAddress();

            // Check for section symbols (start with ".")
            // Section symbols may not have their VA set during symbol resolution, so we need to
            // look up the actual input section and use its VA.
            // NOTE: The input section's VA is already correctly positioned in the output section
            // (including any import table/TVector prefix), so we do NOT add the prefix again.
            if (sym->getName().starts_with(".") && symbolVA == 0) {
              // This is likely a section symbol - find the corresponding input section
              InputFile *symFile = sym->getFile();

              // Search for the input section in the owning file
              bool found = false;
              if (auto *elfFile = dyn_cast<ELFObjFile>(symFile)) {
                for (InputSection *sec : elfFile->getInputSections()) {
                  if (sec->getName() == sym->getName()) {
                    symbolVA = sec->getVirtualAddress();
                    // Input section VA is already correctly positioned - do NOT add prefix
                    found = true;
                    break;
                  }
                }
              }

              if (!found) {
                if (config->verbose) {
                  errorHandler().outs() << "  Warning: Could not find section for symbol: "
                                       << sym->getName() << "\n";
                }
                continue;
              }
            }

            // Symbol VA relative to data section base
            tocOffset = static_cast<int32_t>(symbolVA - dataSectionVA);
          } else {
            // For undefined symbols, this shouldn't happen for data access
            if (config->verbose) {
              errorHandler().outs() << "  Warning: R_PPC_ADDR16 for undefined symbol: "
                                   << sym->getName() << "\n";
            }
            continue;
          }

          // Add the addend from the relocation
          tocOffset += reloc.addend;

          // Check if offset fits in signed 16 bits
          if (tocOffset < -32768 || tocOffset > 32767) {
            // Find the input file that contains this symbol for better diagnostics
            StringRef fileName = sym->getFile() ? sym->getFile()->getName() : "<unknown>";
            error("TOC offset overflow for data symbol '" + sym->getName() +
                  "': offset " + Twine(tocOffset) + " exceeds 16-bit signed range [-32768, 32767].\n"
                  "  Symbol location: " + fileName + "\n"
                  "  This usually means large static data tables exceed the 32KB limit.\n"
                  "  Linker has reordered sections (small first), but total data size is too large.\n"
                  "  Consider:\n"
                  "  - Reducing static data size in the linked libraries\n"
                  "  - Splitting large lookup tables into separate compilation units\n"
                  "  - For Rust: try --release or reducing monomorphization");
            continue;
          }

          // Patch the low 16 bits of the instruction
          // The relocation points to the immediate field (last 2 bytes of a 4-byte instruction)
          code[reloc.offset + 0] = (tocOffset >> 8) & 0xFF;
          code[reloc.offset + 1] = tocOffset & 0xFF;

          relocsProcessed++;

          if (config->verbose) {
            errorHandler().outs() << "  R_PPC_ADDR16 at 0x" << utohexstr(reloc.offset)
                                 << " -> " << sym->getName()
                                 << " TOC offset=" << tocOffset << "\n";
          }
        }
        // Handle R_PPC_ADDR16_LO (type 4) - Low 16 bits of TOC-relative address
        // Used with R_PPC_ADDR16_HA for 32-bit addressing (Large code model)
        // Generated by: addi rD, rA, symbol@l
        else if (reloc.type == 4) {
          // VERY EARLY DEBUG
          errorHandler().outs() << "  ENTERING TYPE4 HANDLER: offset=0x" << utohexstr(reloc.offset) << "\n";

          if (reloc.offset + 2 > code.size()) {
            error("relocation offset out of bounds");
            continue;
          }

          if (!sym->isDefined()) {
            if (config->verbose) {
              errorHandler().outs() << "  Warning: R_PPC_ADDR16_LO for undefined symbol: "
                                   << sym->getName() << "\n";
            }
            continue;
          }

          Defined *def = cast<Defined>(sym);
          int32_t fullOffset = 0;

          // DEBUG: Print symbol class for ALL type 4 relocations
          errorHandler().outs() << "  DEBUG TYPE4: sym=\"" << sym->getName()
                               << "\" symbolClass=" << static_cast<int>(def->getSymbolClass())
                               << " (kPEFCodeSymbol=" << static_cast<int>(PEF::kPEFCodeSymbol)
                               << ") addend=0x" << utohexstr(reloc.addend) << "\n";

          // Check if this is a code symbol (function) being used as a function pointer
          // In CFM/PEF, function pointers are TVector addresses, not code addresses
          // HOWEVER, code-to-code references (jump tables, switch statements) should
          // use code addresses directly, not TVectors.
          if (def->getSymbolClass() == PEF::kPEFCodeSymbol) {
            auto tvIt = functionTVectors.find(def);
            if (tvIt != functionTVectors.end()) {
              // Function has TVector - use the TVector offset (function pointer case)
              fullOffset = static_cast<int32_t>(tvIt->second) + reloc.addend;
              if (config->verbose) {
                errorHandler().outs() << "  R_PPC_ADDR16_LO at 0x" << utohexstr(reloc.offset)
                                     << " -> " << sym->getName()
                                     << " (function pointer -> TVector at offset "
                                     << fullOffset << ")\n";
              }
            } else {
              // No named function TVector - check if this is an anonymous function
              // detected via HA/LO pair scanning (like core::fmt formatter functions)
              uint64_t targetCodeAddr = reloc.addend;  // For section symbols, addend is the offset

              // DEBUG: Unconditionally print for code symbols
              errorHandler().outs() << "  DEBUG R_PPC_ADDR16_LO CODE: " << sym->getName()
                                   << "+0x" << utohexstr(targetCodeAddr)
                                   << " vtableCodeAddressToTVector.size()="
                                   << vtableCodeAddressToTVector.size() << "\n";

              auto anonIt = vtableCodeAddressToTVector.find(targetCodeAddr);

              if (anonIt != vtableCodeAddressToTVector.end()) {
                // Found in anonymous function TVector map - use the TVector offset
                fullOffset = static_cast<int32_t>(anonIt->second);
                if (config->verbose) {
                  errorHandler().outs() << "  R_PPC_ADDR16_LO at 0x" << utohexstr(reloc.offset)
                                       << " -> " << sym->getName() << "+0x" << utohexstr(reloc.addend)
                                       << " (anonymous function -> TVector at offset "
                                       << fullOffset << ")\n";
                }
              } else {
                // No TVector - this is a code-to-code reference (jump table, switch, etc.)
                // Use code address directly. The relocation is r2-relative, but we're
                // computing an absolute address that will be stored/loaded.
                uint64_t codeAddr = def->getVirtualAddress();

                // For section symbols like .text, look up the actual section VA
                if (sym->getName().starts_with(".") && codeAddr == 0) {
                  InputFile *symFile = sym->getFile();
                  if (auto *elfFile = dyn_cast<ELFObjFile>(symFile)) {
                    for (InputSection *sec : elfFile->getInputSections()) {
                      if (sec->getName() == sym->getName()) {
                        codeAddr = sec->getVirtualAddress();
                        break;
                      }
                    }
                  }
                }

                // Code addresses are absolute, not relative to data section
                // The instruction will use addis/addi to form the full 32-bit address
                fullOffset = static_cast<int32_t>(codeAddr) + reloc.addend;

                if (config->verbose) {
                  errorHandler().outs() << "  R_PPC_ADDR16_LO at 0x" << utohexstr(reloc.offset)
                                       << " -> " << sym->getName()
                                       << " (code-to-code ref, addr=0x" << utohexstr(codeAddr)
                                       << ", full_offset=" << fullOffset
                                       << ", vtableMap.size=" << vtableCodeAddressToTVector.size()
                                       << ", addend=0x" << utohexstr(reloc.addend) << ")\n";
                }
              }
            }
          } else {
            // Data symbol - use VA relative to data section
            uint64_t symbolVA = def->getVirtualAddress();

            // Handle section symbols (names starting with ".") which need VA adjustment
            // Section symbols may not have their VA set during symbol resolution, so we need to
            // look up the actual input section and use its VA.
            // NOTE: The input section's VA is already correctly positioned in the output section
            // (including any import table/TVector prefix), so we do NOT add the prefix again.
            if (sym->getName().starts_with(".") && symbolVA == 0) {
              InputFile *symFile = sym->getFile();
              bool found = false;
              if (auto *elfFile = dyn_cast<ELFObjFile>(symFile)) {
                for (InputSection *sec : elfFile->getInputSections()) {
                  if (sec->getName() == sym->getName()) {
                    symbolVA = sec->getVirtualAddress();
                    // Input section VA is already correctly positioned - do NOT add prefix
                    found = true;
                    break;
                  }
                }
              }
              if (!found) {
                if (config->verbose) {
                  errorHandler().outs() << "  Warning: R_PPC_ADDR16_LO could not find section for symbol: "
                                       << sym->getName() << "\n";
                }
                continue;
              }
            }

            fullOffset = static_cast<int32_t>(symbolVA - dataSectionVA) + reloc.addend;
          }

          // Extract low 16 bits
          int16_t lo = fullOffset & 0xFFFF;

          // Patch the low 16 bits of the instruction
          code[reloc.offset + 0] = (lo >> 8) & 0xFF;
          code[reloc.offset + 1] = lo & 0xFF;

          relocsProcessed++;

          if (config->verbose && def->getSymbolClass() != PEF::kPEFCodeSymbol) {
            errorHandler().outs() << "  R_PPC_ADDR16_LO at 0x" << utohexstr(reloc.offset)
                                 << " -> " << sym->getName()
                                 << " full_offset=" << fullOffset
                                 << " lo=0x" << utohexstr(lo & 0xFFFF) << "\n";
          }
        }
        // Handle R_PPC_ADDR16_HA (type 6) - High Adjusted 16 bits of TOC-relative address
        // Used with R_PPC_ADDR16_LO for 32-bit addressing (Large code model)
        // Generated by: addis rD, r2, symbol@ha
        // The "adjusted" means we add 0x8000 before shifting to handle sign extension
        else if (reloc.type == 6) {
          if (reloc.offset + 2 > code.size()) {
            error("relocation offset out of bounds");
            continue;
          }

          if (!sym->isDefined()) {
            if (config->verbose) {
              errorHandler().outs() << "  Warning: R_PPC_ADDR16_HA for undefined symbol: "
                                   << sym->getName() << "\n";
            }
            continue;
          }

          Defined *def = cast<Defined>(sym);
          int32_t fullOffset = 0;

          // Check if this is a code symbol (function) being used as a function pointer
          // In CFM/PEF, function pointers are TVector addresses, not code addresses
          // HOWEVER, code-to-code references (jump tables, switch statements) should
          // use code addresses directly, not TVectors.
          if (def->getSymbolClass() == PEF::kPEFCodeSymbol) {
            auto tvIt = functionTVectors.find(def);
            if (tvIt != functionTVectors.end()) {
              // Function has TVector - use the TVector offset (function pointer case)
              fullOffset = static_cast<int32_t>(tvIt->second) + reloc.addend;
              if (config->verbose) {
                errorHandler().outs() << "  R_PPC_ADDR16_HA at 0x" << utohexstr(reloc.offset)
                                     << " -> " << sym->getName()
                                     << " (function pointer -> TVector at offset "
                                     << fullOffset << ")\n";
              }
            } else {
              // No named function TVector - check if this is an anonymous function
              // detected via HA/LO pair scanning (like core::fmt formatter functions)
              uint64_t targetCodeAddr = reloc.addend;  // For section symbols, addend is the offset
              auto anonIt = vtableCodeAddressToTVector.find(targetCodeAddr);
              if (anonIt != vtableCodeAddressToTVector.end()) {
                // Found in anonymous function TVector map - use the TVector offset
                fullOffset = static_cast<int32_t>(anonIt->second);
                if (config->verbose) {
                  errorHandler().outs() << "  R_PPC_ADDR16_HA at 0x" << utohexstr(reloc.offset)
                                       << " -> " << sym->getName() << "+0x" << utohexstr(reloc.addend)
                                       << " (anonymous function -> TVector at offset "
                                       << fullOffset << ")\n";
                }
              } else {
                // No TVector - this is a code-to-code reference (jump table, switch, etc.)
                // Use code address directly. The relocation is r2-relative, but we're
                // computing an absolute address that will be stored/loaded.
                uint64_t codeAddr = def->getVirtualAddress();

                // For section symbols like .text, look up the actual section VA
                if (sym->getName().starts_with(".") && codeAddr == 0) {
                  InputFile *symFile = sym->getFile();
                  if (auto *elfFile = dyn_cast<ELFObjFile>(symFile)) {
                    for (InputSection *sec : elfFile->getInputSections()) {
                      if (sec->getName() == sym->getName()) {
                        codeAddr = sec->getVirtualAddress();
                        break;
                      }
                    }
                  }
                }

                // Code addresses are absolute, not relative to data section
                // The instruction will use addis/addi to form the full 32-bit address
                fullOffset = static_cast<int32_t>(codeAddr) + reloc.addend;

                if (config->verbose) {
                  errorHandler().outs() << "  R_PPC_ADDR16_HA at 0x" << utohexstr(reloc.offset)
                                       << " -> " << sym->getName()
                                       << " (code-to-code ref, addr=0x" << utohexstr(codeAddr)
                                       << ", full_offset=" << fullOffset << ")\n";
                }
              }
            }
          } else {
            // Data symbol - use VA relative to data section
            uint64_t symbolVA = def->getVirtualAddress();

            // Handle section symbols (names starting with ".") which need VA adjustment
            // Section symbols may not have their VA set during symbol resolution, so we need to
            // look up the actual input section and use its VA.
            // NOTE: The input section's VA is already correctly positioned in the output section
            // (including any import table/TVector prefix), so we do NOT add the prefix again.
            if (sym->getName().starts_with(".") && symbolVA == 0) {
              InputFile *symFile = sym->getFile();
              bool found = false;
              if (auto *elfFile = dyn_cast<ELFObjFile>(symFile)) {
                for (InputSection *sec : elfFile->getInputSections()) {
                  if (sec->getName() == sym->getName()) {
                    symbolVA = sec->getVirtualAddress();
                    // Input section VA is already correctly positioned - do NOT add prefix
                    found = true;
                    break;
                  }
                }
              }
              if (!found) {
                if (config->verbose) {
                  errorHandler().outs() << "  Warning: R_PPC_ADDR16_HA could not find section for symbol: "
                                       << sym->getName() << "\n";
                }
                continue;
              }
            }

            fullOffset = static_cast<int32_t>(symbolVA - dataSectionVA) + reloc.addend;
          }

          // Calculate high adjusted: (offset + 0x8000) >> 16
          // The +0x8000 compensates for sign extension when the low part is added
          int16_t ha = (fullOffset + 0x8000) >> 16;

          // Patch the low 16 bits of the instruction (immediate field)
          code[reloc.offset + 0] = (ha >> 8) & 0xFF;
          code[reloc.offset + 1] = ha & 0xFF;

          relocsProcessed++;

          if (config->verbose && def->getSymbolClass() != PEF::kPEFCodeSymbol) {
            errorHandler().outs() << "  R_PPC_ADDR16_HA at 0x" << utohexstr(reloc.offset)
                                 << " -> " << sym->getName()
                                 << " full_offset=" << fullOffset
                                 << " ha=0x" << utohexstr(ha & 0xFFFF) << "\n";
          }
        }
        // Handle R_PPC_ADDR32 (type 1) - 32-bit absolute address
        // Used for data relocations (e.g., pointers in .rodata)
        else if (reloc.type == 1) {
          // This will be handled by PEF runtime relocations, skip for now
          if (config->verbose) {
            errorHandler().outs() << "  R_PPC_ADDR32 at 0x" << utohexstr(reloc.offset)
                                 << " -> " << sym->getName() << " (deferred to PEF reloc)\n";
          }
        }
        else {
          if (config->verbose) {
            errorHandler().outs() << "  Skipping unsupported reloc type "
                                 << reloc.type << " at offset 0x"
                                 << utohexstr(reloc.offset) << "\n";
          }
        }
      }
    }
  }

  if (config->verbose) {
    errorHandler().outs() << "  Processed " << relocsProcessed << " ELF relocations\n";
  }

  // NOTE: Vtable patching (data section R_PPC_ADDR32 relocations) is now handled
  // by patchVTableEntries() which runs BEFORE pattern encoding in assignFileOffsets().
  // This ensures patched vtable data is included in the pattern-encoded output.
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

  // Collect which functions have their address taken (for sparse TVector generation)
  // This prevents TOC overflow for large binaries like Rust with core/alloc
  collectAddressTakenFunctions();

  // Create TVectors for address-taken functions only (for function pointers)
  // Must be done after assignSymbolAddresses() so sort order is correct
  // This populates codeFunctions vector needed for TVector writing
  createFunctionTVectors();

  // VTABLE FIX: Patch vtable function pointers BEFORE pattern encoding
  // This must happen before assignFileOffsets() so the patched data is included
  // in the pattern-encoded data section output.
  patchVTableEntries();

  // Re-assign file offsets now that we know the TVector table size
  // This writes the actual TVector data using the sorted codeFunctions vector
  finalFileOffsetPass = true;  // Set flag so data section encoding happens with correct addresses
  assignFileOffsets();

  // Re-assign symbol addresses now that we know the actual TVector table size
  // This is needed because the first assignSymbolAddresses() used a minimum size estimate
  assignSymbolAddresses();

  // BUG FIX #10 & #15: Update TVect TOC address and offset after assignFileOffsets
  updateEntryPointTVect();

  // Generate import stubs in code section
  generateImportStubs();

  // Generate TOC entries in data section
  generateTOCEntries();

  // Assign virtual addresses to imported symbols
  assignImportAddresses();

  // Replace bl .+1 instructions with calls to import stubs (PEF input files)
  replaceImportCalls();

  // Process ELF relocations (R_PPC_REL24 for function calls)
  processELFRelocations();

  // Optimize TOC restores for same-fragment calls (Mac OS Classic CFM)
  optimizeTOCRestores();

  // BUG FIX #15: Create loader section AFTER tvectOffset is finalized
  createLoaderSection();

  // Recalculate file offsets using actual loader size
  // This is the key to dynamic loader sizing - no hardcoded limits!
  recalculateFileOffsets();

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
