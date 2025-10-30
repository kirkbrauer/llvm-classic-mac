//===- RelocWriter.cpp - PEF Relocation Bytecode Generator ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "RelocWriter.h"
#include "Config.h"
#include "InputSection.h"
#include "OutputSection.h"
#include "Symbols.h"
#include "lld/Common/ErrorHandler.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"

using namespace llvm;
using namespace llvm::PEF;
using namespace llvm::support;
using namespace lld;
using namespace lld::pef;

PEFRelocWriter::PEFRelocWriter(
    const std::vector<OutputSection *> &sections,
    const std::vector<ImportedLibraryInfo> &imports)
    : outputSections(sections), importedLibraries(imports) {

  // Pre-set common section indices
  for (size_t i = 0; i < sections.size(); ++i) {
    uint8_t kind = sections[i]->getKind();
    if (kind == kPEFCodeSection && sectionC == -1) {
      sectionC = i;
    } else if (kind == kPEFUnpackedDataSection && sectionD == -1) {
      sectionD = i;
    }
  }
}

std::pair<std::vector<uint8_t>, std::vector<uint8_t>>
PEFRelocWriter::generate() {
  if (config->verbose) {
    errorHandler().outs() << "\nGenerating relocation instructions...\n";
  }

  // Process each output section that has relocations
  for (size_t i = 0; i < outputSections.size(); ++i) {
    processSection(outputSections[i], i);
  }

  // Optimize instruction stream (Phase 3.3)
  optimize();

  // Convert to byte arrays
  std::vector<uint8_t> headerBytes;
  std::vector<uint8_t> instrBytes;

  // Write headers (12 bytes each)
  for (const auto &header : headers) {
    uint8_t buf[12];
    endian::write16be(buf + 0, header.SectionIndex);
    endian::write16be(buf + 2, header.ReservedA);
    endian::write32be(buf + 4, header.RelocCount);
    endian::write32be(buf + 8, header.FirstRelocOffset);
    headerBytes.insert(headerBytes.end(), buf, buf + 12);
  }

  // Write instructions (2 bytes each, big-endian)
  for (uint16_t instr : instructions) {
    uint8_t buf[2];
    endian::write16be(buf, instr);
    instrBytes.insert(instrBytes.end(), buf, buf + 2);
  }

  if (config->verbose) {
    errorHandler().outs() << "  Generated " << headers.size()
                         << " relocation headers\n";
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

  // Reset position for new section
  relocAddress = 0;
  bool needSetPosition = true;

  // Process relocations from all input sections
  for (InputSection *isec : osec->getInputSections()) {
    ArrayRef<uint16_t> inputRelocs = isec->getRelocations();
    if (inputRelocs.empty())
      continue;

    uint32_t isecBase = isec->getVirtualAddress() - osec->getVirtualAddress();

    if (config->verbose) {
      errorHandler().outs() << "    Processing " << inputRelocs.size()
                           << " relocations from input section at offset 0x"
                           << utohexstr(isecBase) << "\n";
    }

    // Decode and re-encode relocations
    uint32_t pos = isecBase;

    for (size_t i = 0; i < inputRelocs.size(); ) {
      uint16_t instr = endian::read16be(&inputRelocs[i]);
      uint8_t opcode = (instr >> 10) & 0x3F;
      uint16_t operand = instr & 0x3FF;

      switch (opcode) {
        case kPEFRelocBySectC:
        case kPEFRelocBySectD: {
          // Section-relative relocations
          if (needSetPosition || pos != relocAddress) {
            emitSetPosition(pos);
            relocAddress = pos;
            needSetPosition = false;
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
          // Import reference - need to remap import index
          uint32_t oldIndex = operand;
          if (opcode == kPEFRelocLgByImport && i + 1 < inputRelocs.size()) {
            uint16_t instr2 = endian::read16be(&inputRelocs[i + 1]);
            oldIndex = (operand << 16) | instr2;
            i++; // Skip second instruction
          }

          // Set position if needed
          if (needSetPosition || pos != relocAddress) {
            emitSetPosition(pos);
            relocAddress = pos;
            needSetPosition = false;
          }

          // For now, use the same index (proper remapping in Phase 3.4)
          // TODO: Map old import index to new import index
          emitByImport(oldIndex);

          relocAddress += 4;
          pos = relocAddress;
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
  // First instruction: opcode (6 bits) + high 10 bits of offset
  uint16_t instr1 = (kPEFRelocSetPosition << 10) | ((offset >> 16) & 0x3FF);
  // Second instruction: low 16 bits of offset
  uint16_t instr2 = offset & 0xFFFF;

  emitInstruction(instr1);
  emitInstruction(instr2);
}

void PEFRelocWriter::emitBySectC(uint16_t runLength) {
  uint16_t instr = (kPEFRelocBySectC << 10) | (runLength & 0x3FF);
  emitInstruction(instr);
}

void PEFRelocWriter::emitBySectD(uint16_t runLength) {
  uint16_t instr = (kPEFRelocBySectD << 10) | (runLength & 0x3FF);
  emitInstruction(instr);
}

void PEFRelocWriter::emitByImport(uint32_t index) {
  if (index < 256) {
    // Small import index (1 instruction)
    uint16_t instr = (kPEFRelocSmByImport << 10) | (index & 0x3FF);
    emitInstruction(instr);
  } else {
    // Large import index (2 instructions)
    // First instruction: opcode (6 bits) + high 6 bits of index
    uint16_t instr1 = (kPEFRelocLgByImport << 10) | ((index >> 16) & 0x3FF);
    // Second instruction: low 16 bits of index
    uint16_t instr2 = index & 0xFFFF;
    emitInstruction(instr1);
    emitInstruction(instr2);
  }
}

void PEFRelocWriter::emitSetSectC(uint16_t index) {
  uint16_t instr = (kPEFRelocSmSetSectC << 10) | (index & 0x3FF);
  emitInstruction(instr);
}

void PEFRelocWriter::emitSetSectD(uint16_t index) {
  uint16_t instr = (kPEFRelocSmSetSectD << 10) | (index & 0x3FF);
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

void PEFRelocWriter::optimize() {
  // Phase 3.3: Optimization pass
  // TODO: Implement optimizations:
  // 1. Combine consecutive BySectC/D into runs
  // 2. Use SmRepeat for repeated patterns
  // 3. Eliminate redundant SetPosition

  // For now, optimization is deferred to Phase 3.3
}
