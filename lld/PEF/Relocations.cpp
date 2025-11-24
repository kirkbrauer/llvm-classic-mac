//===- Relocations.cpp ----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Relocations.h"
#include "Config.h"
#include "InputFiles.h"
#include "InputSection.h"
#include "Symbols.h"
#include "SymbolTable.h"
#include "lld/Common/ErrorHandler.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Object/PEFObjectFile.h"
#include "llvm/Support/Endian.h"

using namespace llvm;
using namespace llvm::object;
using namespace llvm::support;
using namespace lld;
using namespace lld::pef;

void lld::pef::scanRelocations(InputSection *isec) {
  // For Phase 1: We're linking simple object files without external dependencies
  // so there's nothing to scan yet.
  //
  // For Phase 2: This will scan relocations to mark imported symbols as needed
  // and pull in lazy symbols from archives.

  ObjFile *file = isec->getFile();
  PEFObjectFile *obj = file->getPEFObj();

  // Find the section in the object file
  unsigned targetIdx = isec->getIndex();
  unsigned currentIdx = 0;

  for (SectionRef sec : obj->sections()) {
    if (currentIdx == targetIdx) {
      // Iterate through relocations for this section
      for (const RelocationRef &rel : sec.relocations()) {
        // Phase 2 will process relocations here
        (void)rel; // Suppress unused warning for now
      }
      break;
    }
    currentIdx++;
  }
}

void lld::pef::processRelocations(InputSection *isec) {
  // Apply relocations to patch the code/data during linking
  // This resolves internal references and prepares external references for CFM

  ObjFile *file = isec->getFile();
  PEFObjectFile *obj = file->getPEFObj();

  // Get the section data that we'll be patching
  Expected<ArrayRef<uint8_t>> dataOrErr = isec->getData();
  if (!dataOrErr) {
    error("failed to get section data: " + toString(dataOrErr.takeError()));
    return;
  }

  // Create a mutable copy of the section data
  std::vector<uint8_t> data(dataOrErr->begin(), dataOrErr->end());
  bool hasPatches = false;

  // Get relocation instructions from input section
  ArrayRef<uint16_t> relocInstructions = isec->getRelocations();

  if (relocInstructions.empty()) {
    return;  // No relocations to process
  }

  if (config->verbose) {
    errorHandler().outs() << "  Processing relocations for section " << isec->getName()
                         << " (" << relocInstructions.size() << " instructions)\n";
  }

  // Decode PEF relocation instructions and apply them
  // PEF uses a streaming bytecode model - see docs/PEF_RELOCATIONS.md
  uint32_t relocAddress = 0;  // Current relocation cursor position

  // Helper lambda to patch a branch instruction
  auto patchBranch = [&](uint32_t addr, uint64_t targetAddr) {
    uint64_t sourceAddr = isec->getVirtualAddress() + addr;
    int64_t offset = static_cast<int64_t>(targetAddr) - static_cast<int64_t>(sourceAddr);

    // PowerPC bl: [opcode:6][offset:24][AA:1][LK:1]
    uint32_t branchInstr = (18 << 26) | ((offset & 0x03FFFFFC)) | 1;
    data[addr + 0] = (branchInstr >> 24) & 0xFF;
    data[addr + 1] = (branchInstr >> 16) & 0xFF;
    data[addr + 2] = (branchInstr >> 8) & 0xFF;
    data[addr + 3] = branchInstr & 0xFF;
    hasPatches = true;
  };

  for (size_t i = 0; i < relocInstructions.size(); ++i) {
    // PEF relocation instructions are big-endian 16-bit values
    uint16_t instr = support::endian::read16be(&relocInstructions[i]);
    uint8_t opcode = (instr >> 9) & 0x7F;
    uint16_t operand = instr & 0x1FF;

    if (config->verbose) {
      errorHandler().outs() << "    Instr[" << i << "] = 0x" << utohexstr(instr)
                           << " opcode=0x" << utohexstr(opcode)
                           << " (" << (unsigned)opcode << ")"
                           << " operand=" << operand << "\n";
    }

    using namespace llvm::PEF;

    switch (opcode) {
      case 0x00: {  // RelocBySectDWithSkip
        // Format: [skipCount:5][relocCount:11]
        uint32_t skipCount = (instr >> 11) & 0x1F;
        uint32_t relocCount = instr & 0x7FF;

        relocAddress += skipCount * 4;

        // For now, just advance cursor - section-relative addressing
        // will be resolved when we have actual section bases
        relocAddress += relocCount * 4;

        if (config->verbose) {
          errorHandler().outs() << "      RelocBySectDWithSkip: skip=" << skipCount
                               << " relocate=" << relocCount << "\n";
        }
        break;
      }

      case kPEFRelocBySectC: {  // 0x20
        uint32_t runLength = operand + 1;
        if (config->verbose) {
          errorHandler().outs() << "      BySectC: runLength=" << runLength
                               << " at offset 0x" << utohexstr(relocAddress) << "\n";
        }
        // Section-relative - just advance cursor
        relocAddress += runLength * 4;
        break;
      }

      case kPEFRelocBySectD: {  // 0x21
        uint32_t runLength = operand + 1;
        if (config->verbose) {
          errorHandler().outs() << "      BySectD: runLength=" << runLength
                               << " at offset 0x" << utohexstr(relocAddress) << "\n";
        }
        relocAddress += runLength * 4;
        break;
      }

      case kPEFRelocTVector12: {  // 0x22
        uint32_t count = operand + 1;
        if (config->verbose) {
          errorHandler().outs() << "      TVector12: count=" << count << "\n";
        }
        relocAddress += count * 12;
        break;
      }

      case kPEFRelocTVector8: {  // 0x23
        uint32_t count = operand + 1;
        if (config->verbose) {
          errorHandler().outs() << "      TVector8: count=" << count << "\n";
        }
        relocAddress += count * 8;
        break;
      }

      case kPEFRelocSmRepeat: {  // 0x28
        uint32_t count = operand + 1;
        if (config->verbose) {
          errorHandler().outs() << "      SmRepeat: count=" << count << "\n";
        }
        relocAddress += count * 4;
        break;
      }

      case kPEFRelocSmSetSectC: {  // 0x29
        if (config->verbose) {
          errorHandler().outs() << "      SmSetSectC: index=" << operand << "\n";
        }
        // Section C base changes - note but don't need to track
        break;
      }

      case kPEFRelocSmSetSectD: {  // 0x2A
        if (config->verbose) {
          errorHandler().outs() << "      SmSetSectD: index=" << operand << "\n";
        }
        break;
      }

      case kPEFRelocSmByImport: {  // 0x2B
        uint32_t index = operand;

        // Get the imported symbol by index from the file's import table
        Symbol *sym = file->getImportSymbol(index);
        if (!sym) {
          error("invalid import index " + Twine(index) +
                " in SmByImport relocation at offset 0x" + utohexstr(relocAddress) +
                " (file: " + file->getName() + ")");
          relocAddress += 4;
          break;
        }

        // Check if this symbol is actually defined internally
        Symbol *resolved = symtab->find(sym->getName());
        if (resolved && resolved->isDefined()) {
          // Internal reference - patch the branch instruction
          Defined *def = cast<Defined>(resolved);
          uint64_t targetAddr = def->getVirtualAddress();

          patchBranch(relocAddress, targetAddr);

          if (config->verbose) {
            errorHandler().outs() << "      SmByImport: Patched internal call to '"
                                 << sym->getName() << "' at offset 0x"
                                 << utohexstr(relocAddress) << "\n";
          }
        } else {
          // External import - leave as 0 (CFM will resolve via import stub)
          if (config->verbose) {
            errorHandler().outs() << "      SmByImport: External import '"
                                 << sym->getName() << "' (will be resolved by CFM)\n";
          }
        }

        relocAddress += 4;
        break;
      }

      case kPEFRelocSetPosition: {  // 0x48 - TWO INSTRUCTIONS
        if (i + 1 >= relocInstructions.size()) {
          error("SetPosition missing second instruction");
          break;
        }
        uint16_t instr2 = support::endian::read16be(&relocInstructions[++i]);
        uint32_t offset = (operand << 16) | instr2;

        relocAddress = offset;

        if (config->verbose) {
          errorHandler().outs() << "      SetPosition: offset=0x" << utohexstr(offset) << "\n";
        }
        break;
      }

      case kPEFRelocLgByImport: {  // 0x52 - TWO INSTRUCTIONS
        if (i + 1 >= relocInstructions.size()) {
          error("LgByImport missing second instruction");
          relocAddress += 4;
          break;
        }
        uint16_t instr2 = support::endian::read16be(&relocInstructions[++i]);
        uint32_t index = (operand << 16) | instr2;

        // Get the imported symbol by index from the file's import table
        Symbol *sym = file->getImportSymbol(index);

        if (config->verbose) {
          errorHandler().outs() << "      DEBUG: LgByImport index=" << index
                               << " sym=" << (void*)sym
                               << " file=" << file->getName()
                               << " file_ptr=" << (void*)file << "\n";
        }

        if (!sym) {
          error("invalid import index " + Twine(index) +
                " in LgByImport relocation at offset 0x" + utohexstr(relocAddress) +
                " (file: " + file->getName() + ")");
          relocAddress += 4;
          break;
        }

        // Note: LgByImport relocations are handled by Writer::replaceImportCalls()
        // which runs after this function. That code handles both branch instructions
        // and data references (lis/addi pairs) for internal symbols.
        // We just log what we find here for debugging.
        Symbol *resolved = symtab->find(sym->getName());
        if (resolved && resolved->isDefined()) {
          if (config->verbose) {
            errorHandler().outs() << "      LgByImport: Internal symbol '"
                                 << sym->getName() << "' at offset 0x"
                                 << utohexstr(relocAddress)
                                 << " (will be patched by replaceImportCalls)\n";
          }
        } else {
          if (config->verbose) {
            errorHandler().outs() << "      LgByImport: External import '"
                                 << sym->getName() << "' at offset 0x"
                                 << utohexstr(relocAddress)
                                 << " (will be resolved by CFM)\n";
          }
        }

        relocAddress += 4;
        break;
      }

      default:
        // Unknown/unhandled opcode
        // Check if this might be a 2-instruction opcode (0x48-0x5F range)
        if (opcode >= 0x48 && opcode <= 0x5F) {
          // Large opcode - skip second instruction
          if (i + 1 < relocInstructions.size()) {
            if (config->verbose) {
              uint16_t instr2 = support::endian::read16be(&relocInstructions[i + 1]);
              errorHandler().outs() << "      Unhandled large opcode 0x"
                                   << utohexstr(opcode) << " (2 instructions: 0x"
                                   << utohexstr(instr) << " 0x" << utohexstr(instr2) << ")\n";
            }
            i++;  // Skip second instruction
          }
        } else {
          if (config->verbose) {
            errorHandler().outs() << "      Unhandled relocation opcode 0x"
                                 << utohexstr(opcode) << "\n";
          }
        }
        break;
    }
  }

  // Store patched data if we made changes
  if (hasPatches) {
    isec->setPatchedData(std::move(data));
    if (config->verbose) {
      errorHandler().outs() << "    Applied " << (hasPatches ? "patches" : "no patches") << " to section\n";
    }
  }
}
