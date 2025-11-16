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
  // This is a simplified decoder that handles the basic relocation types we need
  uint32_t relocAddress = 0;  // Current relocation cursor position

  for (size_t i = 0; i < relocInstructions.size(); ++i) {
    uint16_t instr = relocInstructions[i];
    uint8_t opcode = (instr >> 9) & 0x7F;
    uint16_t operand = instr & 0x1FF;

    if (config->verbose) {
      errorHandler().outs() << "    Instr[" << i << "] = 0x" << utohexstr(instr)
                           << " opcode=" << (unsigned)opcode
                           << " operand=" << operand << "\n";
    }

    using namespace llvm::PEF;

    switch (opcode) {
      case kPEFRelocSmByImport:
      case kPEFRelocLgByImport: {
        // Import relocation - check if symbol is internally defined
        uint32_t importIndex = operand;
        if (opcode == kPEFRelocLgByImport && i + 1 < relocInstructions.size()) {
          uint16_t instr2 = relocInstructions[i + 1];
          importIndex = (operand << 16) | instr2;
          i++;  // Skip second instruction
        }

        // Get the imported symbol by index from the file's import table
        Symbol *sym = file->getImportSymbol(importIndex);
        if (!sym) {
          error("invalid import index " + Twine(importIndex));
          continue;
        }

        // Check if this symbol is actually defined internally
        Symbol *resolved = symtab->find(sym->getName());
        if (resolved && resolved->isDefined()) {
          // Internal reference - patch the branch instruction
          Defined *def = cast<Defined>(resolved);

          // Calculate target address (virtual address of symbol)
          uint64_t targetAddr = def->getVirtualAddress();

          // Calculate source address (virtual address of this relocation point)
          uint64_t sourceAddr = isec->getVirtualAddress() + relocAddress;

          // Calculate branch offset (target - source)
          int64_t offset = static_cast<int64_t>(targetAddr) - static_cast<int64_t>(sourceAddr);

          // Patch the branch instruction in the data
          // PowerPC branch: opcode (6 bits) | offset (24 bits) | AA (1) | LK (1)
          // For "bl" (branch and link): opcode = 0x12 (18), AA = 0, LK = 1
          if (relocAddress + 4 <= data.size()) {
            // Encode as big-endian 32-bit instruction
            uint32_t branchInstr = (18 << 26) | ((offset & 0x03FFFFFC)) | 1;
            data[relocAddress + 0] = (branchInstr >> 24) & 0xFF;
            data[relocAddress + 1] = (branchInstr >> 16) & 0xFF;
            data[relocAddress + 2] = (branchInstr >> 8) & 0xFF;
            data[relocAddress + 3] = branchInstr & 0xFF;

            hasPatches = true;

            if (config->verbose) {
              errorHandler().outs() << "    Patched internal call to '" << sym->getName()
                                   << "' at offset 0x" << utohexstr(relocAddress)
                                   << " (target=0x" << utohexstr(targetAddr)
                                   << ", offset=" << offset << ")\n";
            }
          } else {
            error("relocation offset out of bounds");
          }
        } else {
          // External import - leave as 0 (CFM will resolve via import stub)
          if (config->verbose) {
            errorHandler().outs() << "    Skipping external import '" << sym->getName()
                                 << "' (will be resolved by CFM)\n";
          }
        }

        relocAddress += 4;  // Advance to next word
        break;
      }

      case kPEFRelocSmRepeat: {
        // Repeat last relocation N times
        uint32_t count = operand + 1;
        relocAddress += 4 * count;
        break;
      }

      default:
        // Skip other relocation types for now
        if (config->verbose) {
          errorHandler().outs() << "    Skipping relocation opcode " << (unsigned)opcode << "\n";
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
