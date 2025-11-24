//===- RelocWriter.cpp - PEF Relocation Bytecode Generator ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "RelocWriter.h"
#include "Config.h"
#include "InputFiles.h"
#include "InputSection.h"
#include "OutputSection.h"
#include "Symbols.h"
#include "lld/Common/ErrorHandler.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"
#include <algorithm>

using namespace llvm;
using namespace llvm::PEF;
using namespace llvm::support;
using namespace lld;
using namespace lld::pef;

PEFRelocWriter::PEFRelocWriter(
    const std::vector<OutputSection *> &sections,
    const std::vector<ImportedLibraryInfo> &imports,
    uint32_t functionTVectorsSize,
    const std::set<uint32_t> *patchedPositions)
    : outputSections(sections), importedLibraries(imports),
      functionTVectorsSize(functionTVectorsSize),
      patchedPositions(patchedPositions) {

  // Pre-set common section indices
  for (size_t i = 0; i < sections.size(); ++i) {
    uint8_t kind = sections[i]->getKind();
    if (kind == kPEFCodeSection && sectionC == -1) {
      sectionC = i;
    } else if ((kind == kPEFUnpackedDataSection || kind == kPEFPatternDataSection) && sectionD == -1) {
      sectionD = i;
    }
  }
}

std::pair<std::vector<uint8_t>, std::vector<uint8_t>>
PEFRelocWriter::generate() {
  if (config->verbose) {
    errorHandler().outs() << "\nGenerating relocation instructions...\n";
  }

  // BUG FIX #34: Use CodeWarrior's relocation sequence (ImportRun + TVector8)
  // Fix #33's SetPosition approach was wrong - TVector8 patches an 8-byte structure starting
  // at the current cursor position, not at a specific field offset.
  //
  // Data section layout: [Import table (4)][TVect (12)][TOC entries (N*12)]
  //
  // Relocation sequence (matches CodeWarrior):
  //   ImportRun (opcode 0x25, operand = count-1 → count imports)
  //            Patches import table at offset 0 (4*count bytes)
  //            Cursor advances from 0 → 4*count
  //   0x4600 = TVector8 (opcode 0x23, count=0 → 1 TVector)
  //            Patches 8-byte TVector at offset 4-11:
  //              - Offset 4-7 (word 0): += code section base
  //              - Offset 8-11 (word 1): += data section base
  //            Cursor advances from 4 → 12
  //
  // TVect.toc (offset 8-11) is initialized to 16 (offset of TOC entries).
  // After TVector8: TVect.toc = 16 + data_base = address of TOC entries ✓

  // BUG FIX: Emit relocations in CORRECT order by section
  // Code section relocations go in code section header
  // Data section relocations go in data section header with ImportRun FIRST

  // Step 1: Emit DATA section relocations in proper order
  // This ensures ImportRun and TVector8 come BEFORE any BySectD relocations
  uint32_t dataSectionInstrStart = instructions.size();

  // BUG FIX #36: Calculate correct ImportRun count based on actual number of imports
  uint32_t totalImports = 0;
  for (const auto &lib : importedLibraries) {
    totalImports += lib.symbols.size();
  }

  // Emit ImportRun FIRST - this patches import table at offset 0
  if (totalImports > 0) {
    uint16_t importRunInstr = (kPEFRelocImportRun << 9) | ((totalImports - 1) & 0x1FF);
    instructions.push_back(importRunInstr);
    if (config->verbose) {
      errorHandler().outs() << "Emitted ImportRun: 0x" << utohexstr(importRunInstr)
                           << " (count=" << totalImports << ")\n";
    }
  }

  // CRITICAL FIX: Skip padding between import table and TVector using IncrPosition
  // After ImportRun, cursor is at offset (totalImports * 4)
  // We need to skip 8 bytes of padding to get to the TVector at offset (totalImports * 4 + 8)
  // IncrPosition operand is (offset - 1), and offset is in bytes
  // So to skip 8 bytes: operand = 8 - 1 = 7
  uint32_t paddingSize = 8;
  if (paddingSize > 0) {
    uint16_t incrPosInstr = (kPEFRelocIncrPosition << 9) | ((paddingSize - 1) & 0x1FF);
    instructions.push_back(incrPosInstr);
    if (config->verbose) {
      errorHandler().outs() << "Emitted IncrPosition: 0x" << utohexstr(incrPosInstr)
                           << " (skip " << paddingSize << " bytes to TVector)\n";
    }
  }

  // Emit TVector8 relocations for SPARSE TVector layout
  // SPARSE LAYOUT: TVectors are at offsets matching code offsets, with gaps (zeros) between them
  // We emit TVector8 to cover the entire sparse range (functionTVectorsSize bytes)
  // CFM will patch all 8-byte slots in this range, including zero-filled gaps (harmless)
  // TVector8 opcode = kPEFRelocTVector8 = 0x23, shifted left 9 bits
  // Operand = (count - 1), where count is number of 8-byte slots to patch
  // Maximum count per instruction = 512 (9-bit operand: 0-511 = 1-512 slots)
  if (functionTVectorsSize > 0) {
    // Calculate number of 8-byte slots in the sparse range
    uint32_t numSlots = functionTVectorsSize / 8;

    // Emit TVector8 instructions to cover all slots (including gaps)
    uint32_t remainingSlots = numSlots;
    while (remainingSlots > 0) {
      uint32_t batchSize = std::min(remainingSlots, 512u);  // Max 512 slots per instruction
      uint16_t count = batchSize - 1;  // Operand encodes (count - 1)
      uint16_t tvector8Instr = (0x23 << 9) | (count & 0x1FF);
      instructions.push_back(tvector8Instr);

      if (config->verbose) {
        errorHandler().outs() << "Emitted TVector8: 0x" << utohexstr(tvector8Instr)
                             << " (covering " << batchSize << " 8-byte slots in sparse layout)\n";
      }

      remainingSlots -= batchSize;
    }
  }

  // IMPORTANT: PEF section indices are:
  //   0 = Code section (.text)
  //   1 = Data section (.data/.pattern)
  // These are FIXED indices regardless of outputSections array order!
  // Headers MUST be in section index order, and instructions must match!
  const uint16_t kCodeSectionIndex = 0;
  const uint16_t kDataSectionIndex = 1;

  // Step 2: Process DATA section relocations (section 1)
  // The ImportRun + IncrPosition + TVector8 were already emitted above
  // Now process any BySectD relocations from input sections
  for (size_t i = 0; i < outputSections.size(); ++i) {
    OutputSection *osec = outputSections[i];
    uint8_t kind = osec->getKind();
    if (kind == kPEFPatternDataSection || kind == kPEFUnpackedDataSection) {
      // Process any BySectD relocations from input sections
      uint32_t beforeDataRelocs = instructions.size();
      processSection(osec, kDataSectionIndex);  // Use actual PEF section index
      uint32_t dataRelocs = instructions.size() - beforeDataRelocs;

      // Create data section relocation header manually
      // Include ImportRun + TVector8(s) + any BySectD relocs from processSection
      LoaderRelocationHeader dataHeader;
      dataHeader.SectionIndex = kDataSectionIndex;  // Data section = 1
      dataHeader.ReservedA = 0;     // Per PEF spec
      dataHeader.RelocCount = instructions.size() - dataSectionInstrStart;
      dataHeader.FirstRelocOffset = dataSectionInstrStart * 2;  // Byte offset

      // Remove header created by processSection (if any) and add our manual one
      if (!headers.empty() && headers.back().SectionIndex == kDataSectionIndex) {
        headers.pop_back();  // Remove processSection's header
      }
      headers.push_back(dataHeader);

      if (config->verbose) {
        uint32_t totalDataRelocs = instructions.size() - dataSectionInstrStart;
        errorHandler().outs() << "Processed data section: " << dataRelocs
                             << " BySectD relocs, total with ImportRun+TVector8s: "
                             << totalDataRelocs << "\n";
      }
      break;
    }
  }

  // Step 3: Process CODE section relocations (section 0)
  for (size_t i = 0; i < outputSections.size(); ++i) {
    OutputSection *osec = outputSections[i];
    if (osec->getKind() == kPEFCodeSection) {
      processSection(osec, kCodeSectionIndex);  // Use actual PEF section index
      if (config->verbose) {
        errorHandler().outs() << "Processed code section relocations\n";
      }
      break;
    }
  }

  // BUG FIX #29: Skip optimize() and merging - we're using exact CodeWarrior sequence
  // No need to optimize or merge when we're hard-coding the instructions

  // Convert to byte arrays
  std::vector<uint8_t> headerBytes;
  std::vector<uint8_t> instrBytes;

  // Headers should already be in section index order since we process
  // data section (1) first, then code section (0)... wait that's backwards!
  // PEF requires headers in section index order: 0, 1, 2...
  // So we need to swap: code section header first, then data section header
  if (headers.size() == 2 && headers[0].SectionIndex > headers[1].SectionIndex) {
    std::swap(headers[0], headers[1]);
  }

  // Write headers (12 bytes each)
  for (const auto &header : headers) {
    uint8_t buf[12];
    endian::write16be(buf + 0, header.SectionIndex);
    endian::write16be(buf + 2, header.ReservedA);
    endian::write32be(buf + 4, header.RelocCount);
    endian::write32be(buf + 8, header.FirstRelocOffset);
    headerBytes.insert(headerBytes.end(), buf, buf + 12);

    if (config->verbose) {
      errorHandler().outs() << "  Writing reloc header: SectionIndex=" << header.SectionIndex
                           << " RelocCount=" << header.RelocCount
                           << " FirstRelocOffset=0x" << utohexstr(header.FirstRelocOffset) << "\n";
    }
  }

  // Write instructions (2 bytes each, big-endian)
  for (uint16_t instr : instructions) {
    uint8_t buf[2];
    endian::write16be(buf, instr);
    instrBytes.insert(instrBytes.end(), buf, buf + 2);
  }

  if (config->verbose) {
    errorHandler().outs() << "  Generated " << headers.size()
                         << " relocation header(s)\n";
    errorHandler().outs() << "  Generated " << instructions.size()
                         << " relocation instructions ("
                         << (instructions.size() * 2) << " bytes)\n";
  }

  return {std::move(headerBytes), std::move(instrBytes)};
}

void PEFRelocWriter::processSection(OutputSection *osec,
                                    unsigned sectionIndex) {
  // Track start of instructions for this section
  uint32_t instrStart = instructions.size();

  // BUG FIX #8: Emit section-switching instruction at start of each section
  // This is CRITICAL for proper relocation processing!
  // We must tell CFM which section we're applying relocations to.
  if (sectionIndex == 0) {
    // Code section
    if (sectionC != 0) {  // Emit if not already set to code section (0)
      emitSetSectC(0);
      sectionC = 0;
      if (config->verbose) {
        errorHandler().outs() << "  Switching to code section (0)\n";
      }
    }
  } else if (sectionIndex == 1) {
    // Data section
    // BUG FIX: Skip SetSectD for data section if we've already emitted ImportRun+TVector8
    // Those instructions implicitly operate on the data section, so no section switch needed
    // Only emit SetSectD if there are no prior instructions (empty data section relocs)
    if (instructions.empty()) {
      emitSetSectD(0);
      sectionD = 0;
      if (config->verbose) {
        errorHandler().outs() << "  Switching to data section (0)\n";
      }
    }
  }

  // Reset position for new section
  relocAddress = 0;
  bool needSetPosition = true;

  // Process relocations from all input sections
  for (InputSection *isec : osec->getInputSections()) {
    ArrayRef<uint16_t> inputRelocs = isec->getRelocations();
    if (config->verbose) {
      errorHandler().outs() << "    Input section '" << isec->getName()
                           << "' at VA 0x" << utohexstr(isec->getVirtualAddress())
                           << ", size=" << isec->getSize()
                           << ", relocs=" << inputRelocs.size() << "\n";
    }
    if (inputRelocs.empty())
      continue;

    uint32_t isecBase = isec->getVirtualAddress() - osec->getVirtualAddress();

    // DEBUG: Show isecBase calculation for code sections
    if (sectionIndex == 0) {
      errorHandler().outs() << "      DEBUG RelocWriter: Input section '" << isec->getName()
                           << "' isecBase=0x" << utohexstr(isecBase)
                           << " (isec VA=0x" << utohexstr(isec->getVirtualAddress())
                           << " - osec VA=0x" << utohexstr(osec->getVirtualAddress()) << ")\n";
    }

    // BUG FIX: For data sections, input section VAs were assigned BEFORE
    // the import table, padding, and entry point TVector were prepended. Adjust isecBase
    // to account for this offset so relocations patch the correct locations.
    if (sectionIndex == 1) {  // Data section
      // Calculate import table size
      uint32_t importTableSize = 0;
      for (const auto &lib : importedLibraries) {
        importTableSize += lib.symbols.size() * 4;
      }
      // CRITICAL FIX: CodeWarrior model uses padding + ONE TVector (entry point)
      uint32_t paddingSize = 8;  // CodeWarrior adds 8 bytes padding before TVector
      uint32_t entryPointTVectorSize = 8;  // Only the entry point TVector
      isecBase += importTableSize + paddingSize + entryPointTVectorSize;

      if (config->verbose) {
        errorHandler().outs() << "    Adjusting data section base: import="
                             << importTableSize << " + padding=" << paddingSize
                             << " + entryTVect=" << entryPointTVectorSize << " = +"
                             << (importTableSize + paddingSize + entryPointTVectorSize) << "\n";
      }
    }

    if (config->verbose) {
      errorHandler().outs() << "    Processing " << inputRelocs.size()
                           << " relocations from input section at offset 0x"
                           << utohexstr(isecBase) << "\n";
    }

    // Decode and re-encode relocations
    uint32_t pos = isecBase;

    for (size_t i = 0; i < inputRelocs.size(); ) {
      uint16_t instr = endian::read16be(&inputRelocs[i]);
      uint8_t opcode = (instr >> 9) & 0x7F;  // FIX: Shift by 9, not 10!
      uint16_t operand = instr & 0x1FF;      // FIX: 9-bit operand, not 10!

      switch (opcode) {
        case kPEFRelocBySectC:
        case kPEFRelocBySectD: {
          // BUG FIX: Skip BySectD relocations in code section that have already
          // been patched by replaceImportCalls() (code→data references for static data)
          // These were patched during linking and must NOT be re-applied by CFM
          if (opcode == kPEFRelocBySectD && sectionIndex == 0 && patchedPositions) {
            // Check if this position (or any position in the run) was patched
            // NOTE: patchedPositions stores INPUT-section-relative positions
            // but pos is OUTPUT-section-relative, so subtract isecBase
            bool shouldSkip = false;
            uint32_t runLength = operand + 1;

            for (uint32_t j = 0; j < runLength; ++j) {
              uint32_t checkPos = (pos - isecBase) + j * 4;  // Convert to input-section-relative
              if (patchedPositions->count(checkPos)) {
                shouldSkip = true;
                if (config->verbose) {
                  errorHandler().outs() << "        MATCH: Position 0x" << utohexstr(checkPos)
                                       << " is in patchedPositions\n";
                }
                break;
              }
            }

            if (shouldSkip) {
              if (config->verbose) {
                errorHandler().outs() << "      Skipping BySectD at offset 0x"
                                     << utohexstr(pos)
                                     << ", runLength=" << runLength
                                     << " (already patched by linker)\n";
              }
              // Don't emit relocation - just advance position
              pos += 4 * runLength;
              // Note: relocAddress stays where it is - we're creating a gap
              break;
            }
          }

          // Section-relative relocations
          if (needSetPosition || pos != relocAddress) {
            emitSetPosition(pos);
            relocAddress = pos;
            needSetPosition = false;
          }

          if (config->verbose) {
            errorHandler().outs() << "      Emitting "
                                 << (opcode == kPEFRelocBySectC ? "BySectC" : "BySectD")
                                 << " at offset 0x" << utohexstr(pos)
                                 << ", runLength=" << (operand + 1) << "\n";
          }

          // Emit the relocation
          if (opcode == kPEFRelocBySectC) {
            emitBySectC(operand);
          } else {
            emitBySectD(operand);
          }

          // Update position (run length + 1 relocations, each 4 bytes)
          relocAddress += 4 * (operand + 1);
          pos = relocAddress;
          break;
        }

        case kPEFRelocSmByImport:
        case kPEFRelocLgByImport: {
          // BUG FIX #22: Skip import relocations from input sections entirely
          // Import relocations in input object files were markers for "bl .+1" instructions
          // that have ALREADY been replaced with actual calls to import stubs during the
          // "Replacing bl .+1 with import stub calls" phase in Writer.cpp.
          // The import stubs themselves contain the actual import references that CFM will patch.
          // Re-emitting these relocations creates EXTRA bogus import relocations that reference
          // invalid import indices, causing CFM to crash.

          uint32_t localIndex = operand;
          if (opcode == kPEFRelocLgByImport && i + 1 < inputRelocs.size()) {
            uint16_t instr2 = endian::read16be(&inputRelocs[i + 1]);
            localIndex = (operand << 16) | instr2;
            i++; // Skip second instruction
          }

          if (config->verbose) {
            errorHandler().outs() << "      Skipping input import relocation (local index " << localIndex
                                 << ") - already handled during stub call replacement\n";
          }

          // Don't emit anything - the relocation was already handled
          // Position doesn't advance because we're not emitting a relocation
          break;
        }

        case kPEFRelocSetPosition: {
          // Position set - read second instruction
          if (i + 1 < inputRelocs.size()) {
            uint16_t instr2 = endian::read16be(&inputRelocs[i + 1]);
            pos = (operand << 16) | instr2;
            pos += isecBase; // Adjust for section base
            i++; // Skip second instruction
            needSetPosition = true;
          }
          break;
        }

        case kPEFRelocSmSetSectC:
          sectionC = operand;
          emitSetSectC(operand);
          break;

        case kPEFRelocSmSetSectD:
          sectionD = operand;
          emitSetSectD(operand);
          break;

        // Other opcodes - pass through for now
        default:
          emitInstruction(instr);
          break;
      }

      i++;
    }
  }

  // Create header if any instructions were generated
  uint32_t instrCount = instructions.size() - instrStart;
  if (instrCount > 0) {
    LoaderRelocationHeader header;
    header.SectionIndex = sectionIndex;
    header.ReservedA = 0;
    header.RelocCount = instrCount;
    header.FirstRelocOffset = instrStart * 2;  // Byte offset
    headers.push_back(header);

    if (config->verbose) {
      errorHandler().outs() << "  Section " << sectionIndex << " has "
                           << instrCount << " relocation instructions\n";
    }
  }
}

void PEFRelocWriter::emitInstruction(uint16_t instr) {
  instructions.push_back(instr);
}

void PEFRelocWriter::emitSetPosition(uint32_t offset) {
  // Two-instruction sequence for SetPosition
  // PEF format: [opcode:7][operand:9] per Apple's "Mac OS Runtime Architectures"
  // First instruction: opcode (7 bits) + high 9 bits of offset
  uint16_t instr1 = (kPEFRelocSetPosition << 9) | ((offset >> 16) & 0x1FF);
  // Second instruction: low 16 bits of offset
  uint16_t instr2 = offset & 0xFFFF;

  emitInstruction(instr1);
  emitInstruction(instr2);
}

void PEFRelocWriter::emitBySectC(uint16_t runLength) {
  // Format: [opcode:7][runLength:9]
  uint16_t instr = (kPEFRelocBySectC << 9) | (runLength & 0x1FF);
  emitInstruction(instr);
}

void PEFRelocWriter::emitBySectD(uint16_t runLength) {
  // Format: [opcode:7][runLength:9]
  uint16_t instr = (kPEFRelocBySectD << 9) | (runLength & 0x1FF);
  emitInstruction(instr);
}

void PEFRelocWriter::emitByImport(uint32_t index) {
  if (index < 256) {
    // Small import index (1 instruction)
    // Format: [opcode:7][index:9]
    uint16_t instr = (kPEFRelocSmByImport << 9) | (index & 0x1FF);
    emitInstruction(instr);
  } else {
    // Large import index (2 instructions)
    // First instruction: [opcode:7][index_high:9]
    uint16_t instr1 = (kPEFRelocLgByImport << 9) | ((index >> 16) & 0x1FF);
    // Second instruction: [index_low:16]
    uint16_t instr2 = index & 0xFFFF;
    emitInstruction(instr1);
    emitInstruction(instr2);
  }
}

void PEFRelocWriter::emitSetSectC(uint16_t index) {
  // Format: [opcode:7][index:9]
  uint16_t instr = (kPEFRelocSmSetSectC << 9) | (index & 0x1FF);
  emitInstruction(instr);
}

void PEFRelocWriter::emitSetSectD(uint16_t index) {
  // Format: [opcode:7][index:9]
  uint16_t instr = (kPEFRelocSmSetSectD << 9) | (index & 0x1FF);
  emitInstruction(instr);
}

uint32_t PEFRelocWriter::getImportIndex(const Symbol *sym) const {
  // Search in imported libraries
  uint32_t index = 0;
  for (const auto &lib : importedLibraries) {
    for (const auto *s : lib.symbols) {
      if (s == sym)
        return index;
      index++;
    }
  }

  return 0; // Not found - shouldn't happen
}

void PEFRelocWriter::generateImportTableRelocations() {
  // Phase 7: Generate relocations for import address table AND TOC entries
  // Each import needs TWO sets of relocations:
  // 1. Import table slot: patched by CFM with imported symbol's TVect address (ByImport)
  // 2. TOC entry: contains pointer to import table slot (BySectD)

  if (importedLibraries.empty()) {
    return;
  }

  // Find the data section (can be either Unpacked or Pattern data)
  int dataSecIndex = -1;
  for (size_t i = 0; i < outputSections.size(); ++i) {
    uint8_t kind = outputSections[i]->getKind();
    if (kind == kPEFUnpackedDataSection || kind == kPEFPatternDataSection) {
      dataSecIndex = i;
      break;
    }
  }

  if (dataSecIndex < 0) {
    return;  // No data section
  }

  if (config->verbose) {
    errorHandler().outs() << "\nGenerating import table and TOC relocations...\n";
  }

  // Track the start of instructions for this section
  uint32_t instrStart = instructions.size();

  // Set section D if needed
  if (sectionD != dataSecIndex) {
    emitSetSectD(dataSecIndex);
  }

  // PART 1: Generate relocations for import address table (at offset 0)
  // Set position to start of data section (offset 0)
  emitSetPosition(0);
  relocAddress = 0;

  // Count total imports
  uint32_t totalImports = 0;
  for (const auto &lib : importedLibraries) {
    totalImports += lib.symbols.size();
  }

  // Emit one ByImport relocation for each imported symbol
  uint32_t globalIndex = 0;
  for (const auto &lib : importedLibraries) {
    for (size_t i = 0; i < lib.symbols.size(); i++) {
      emitByImport(globalIndex);
      relocAddress += 4;
      globalIndex++;

      if (config->verbose) {
        errorHandler().outs() << "  Import table slot " << globalIndex - 1
                             << " for " << lib.symbols[i]->getName()
                             << " at offset 0x" << utohexstr(relocAddress - 4) << "\n";
      }
    }
  }

  // PART 2: Generate relocations for TOC entries (after import table and TVect)
  // Each TOC entry is 12 bytes: [ptr_to_import_slot, toc_value, reserved]
  // The first word needs a BySectD relocation to point to the import table slot

  if (config->verbose) {
    errorHandler().outs() << "\nGenerating TOC entry relocations...\n";
  }

  // BUG FIX #23: TOC entries start after import table AND TVect (12 bytes)
  // Data layout: [Import table][TVect][TOC entries]
  uint32_t importTableSize = totalImports * 4;
  uint32_t tocEntriesOffset = importTableSize + 12;  // After import table + TVect

  // Set position to start of TOC entries
  emitSetPosition(tocEntriesOffset);
  relocAddress = tocEntriesOffset;

  // For each TOC entry, emit a BySectD relocation for the first word
  for (uint32_t i = 0; i < totalImports; i++) {
    // The TOC entry's first word should point to import table slot i
    // which is at offset (i * 4) in the data section
    // Since the value in the binary is already 0, and we want it to point
    // to data_section_base + (i * 4), we use BySectD which adds data section base
    // But wait - the value needs to be (i * 4), not 0!
    // This requires the TOC entries to be pre-initialized, not zeros.
    // For now, emit BySectD relocation and we'll fix initialization separately
    emitBySectD(0);  // Run length = 0 means 1 relocation
    relocAddress += 4;

    // Skip the other 2 words of the TOC entry (8 bytes)
    // These don't need relocations (toc_value and reserved are 0)
    if (i < totalImports - 1) {
      emitSetPosition(relocAddress + 8);
      relocAddress += 8;
    }

    if (config->verbose) {
      const char *symName = "unknown";
      uint32_t symIdx = 0;
      for (const auto &lib : importedLibraries) {
        if (i < symIdx + lib.symbols.size()) {
          symName = lib.symbols[i - symIdx]->getName().data();
          break;
        }
        symIdx += lib.symbols.size();
      }
      errorHandler().outs() << "  TOC entry " << i
                           << " for " << symName
                           << " at offset 0x" << utohexstr(tocEntriesOffset + i * 12)
                           << " (points to import slot at 0x" << utohexstr(i * 4) << ")\n";
    }
  }

  // BUG FIX #26: Emit RelocDone to terminate the relocation sequence
  // PEF spec requires all relocation sequences to end with RelocDone (0x4000)
  // This is kPEFRelocSmRepeat with count=0, which means "done"
  instructions.push_back(0x4000);

  if (config->verbose) {
    errorHandler().outs() << "  Emitted RelocDone instruction (0x4000)\n";
  }

  // Create relocation header for data section (or update existing)
  // Check if we already have a header for this section
  bool foundHeader = false;
  for (auto &header : headers) {
    if (header.SectionIndex == dataSecIndex) {
      // Update existing header
      header.RelocCount += (instructions.size() - instrStart);
      foundHeader = true;
      if (config->verbose) {
        errorHandler().outs() << "  Updated data section relocation header: "
                             << (instructions.size() - instrStart) << " import relocations added\n";
      }
      break;
    }
  }

  if (!foundHeader) {
    // Create new header
    PEF::LoaderRelocationHeader header;
    header.SectionIndex = dataSecIndex;
    header.ReservedA = 0;
    header.RelocCount = instructions.size() - instrStart;
    header.FirstRelocOffset = instrStart * 2;  // Byte offset
    headers.push_back(header);

    if (config->verbose) {
      errorHandler().outs() << "  Created data section relocation header: "
                           << header.RelocCount << " relocations\n";
    }
  }
}

void PEFRelocWriter::optimize() {
  // Phase 3.3: Optimization pass
  // TODO: Implement optimizations:
  // 1. Combine consecutive BySectC/D into runs
  // 2. Use SmRepeat for repeated patterns
  // 3. Eliminate redundant SetPosition

  // For now, optimization is deferred to Phase 3.3
}
