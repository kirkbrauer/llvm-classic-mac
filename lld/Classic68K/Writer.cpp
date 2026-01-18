//===- Writer.cpp - Classic 68K application writer ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Writer.h"
#include "CodeSegment.h"
#include "Config.h"
#include "DataSection.h"
#include "ELFReader.h"
#include "JumpTable.h"
#include "ResourceWriter.h"
#include "SizeResource.h"
#include "SymbolTable.h"
#include "TrapDatabase.h"
#include "TrapStubs.h"
#include "lld/Common/ErrorHandler.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace lld;

namespace lld::classic68k {

// Startup prologue for Classic 68K applications
// This is prepended to the user's code and becomes the entry point (__start)
//
// The startup code must:
// 1. Zero the entire below-A5 area (BSS initialization - required by C runtime)
// 2. Load DATA 0 resource and copy initialized data over the zeroed area
// 3. Release DATA 0 resource
// 4. Call main()
// 5. Exit via ExitToShell
//
// NOTE: BSS zeroing is CRITICAL. Uninitialized globals (like QuickDraw's qd)
// must be zero before use. Without this, InitGraf(&qd.thePort) crashes because
// qd contains garbage pointers.
//
// __start:
//     ; Save registers we'll use
//     movem.l d0-d2/a0-a2, -(sp)  ; 48E7 E0E0 - Save D0-D2, A0-A2
//
//     ; ===== PHASE 1: ZERO BSS (entire below-A5 region) =====
//     ; This initializes all uninitialized globals to zero as required by C
//     movea.l a5, a0              ; 204D - a0 = A5
//     suba.l  #belowA5Size, a0    ; 91FC XXXXXXXX - a0 = A5 - belowA5Size [PATCH1]
//     moveq   #0, d0              ; 7000 - d0 = 0 (fill value)
// zeroLoop:
//     cmpa.l  a5, a0              ; B1CD - compare a0 to A5
//     bge.s   zeroDone            ; 6C04 - if a0 >= A5, done zeroing
//     move.b  d0, (a0)+           ; 10C0 - *a0++ = 0
//     bra.s   zeroLoop            ; 60F8 - loop back
// zeroDone:
//
//     ; ===== PHASE 2: COPY DATA 0 (initialized globals) =====
//     ; GetResource('DATA', 0) -> handle in A0
//     subq.l  #4, sp              ; 594F - Space for return value
//     move.l  #'DATA', -(sp)      ; 2F3C 44415441 - Push 'DATA'
//     clr.w   -(sp)               ; 4267 - Push ID 0
//     _GetResource                ; A9A0
//     movea.l (sp)+, a0           ; 205F - Pop handle into A0
//
//     ; Check if resource loaded
//     move.l  a0, d0              ; 2008 - Test handle
//     beq.s   skipCopy            ; 67XX - Skip if null (no DATA resource)
//
//     ; Lock handle: HLock(handle)
//     move.l  a0, -(sp)           ; 2F08 - Push handle
//     _HLock                      ; A029
//     addq.l  #4, sp              ; 584F - Pop argument
//
//     ; Dereference handle to get data pointer
//     movea.l (a0), a1            ; 2250 - A1 = *handle (data pointer)
//
//     ; Get data size (first 4 bytes of DATA resource = copy size)
//     move.l  (a1)+, d1           ; 2219 - D1 = size, A1 points to data
//
//     ; Calculate destination: A5 - belowA5Size (start of below-A5 area)
//     movea.l a5, a2              ; 244D - A2 = A5
//     suba.l  #belowA5Size, a2    ; 95FC XXXXXXXX - [PATCH2]
//
//     ; Test if size is zero (avoid infinite loop)
//     tst.l   d1                  ; 4A81 - Test D1
//     beq.s   skipLoop            ; 6706 - Skip loop if zero
//
//     ; Copy D1 bytes from A1 to A2
// copyLoop:
//     move.b  (a1)+, (a2)+        ; 14D9 - Copy byte
//     subq.l  #1, d1              ; 5381 - Decrement count
//     bne.s   copyLoop            ; 66FA - Loop until done
//
// skipLoop:
//     ; ReleaseResource(handle) - Pascal trap, pops its own argument
//     move.l  a0, -(sp)           ; 2F08 - Push handle (still in A0)
//     _ReleaseResource            ; A9A3 - Pascal: pops 4 bytes
//     ; NO cleanup needed - Pascal trap already popped the argument
//     ; NO GetResource space to pop - it was consumed by MOVEA.L (SP)+, A0 earlier
//     bra.s   toRestore           ; 6002
//
// skipCopy:
//     ; NO cleanup needed - GetResource space was consumed by MOVEA.L (SP)+, A0
//
// toRestore:
//     ; Restore saved registers
//     movem.l (sp)+, d0-d2/a0-a2  ; 4CDF 0707 - Restore D0-D2, A0-A2
//
//     ; Call main
//     bsr.w   _main               ; 6100 XXXX - PC-relative call to main
//
//     ; Exit to shell with return code
//     move.w  d0, -(sp)           ; 3F00 - Push return code
//     _ExitToShell                ; A9F4 - Exit
//
// Total size: 92 bytes (0x5C)
// Patch locations:
//   - Below-A5 size at offset BELOW_A5_PATCH_OFFSET1 (for BSS zeroing)
//   - Below-A5 size at offset BELOW_A5_PATCH_OFFSET2 (for DATA copy)
//   - BSR displacement at offset BSR_PATCH_OFFSET
//
static const uint8_t startupPrologue[] = {
    // Save registers: MOVEM.L D0-D2/A0-A2, -(SP)
    0x48, 0xE7, 0xE0, 0xE0,  // +0x00: 48E7 E0E0

    // ===== PHASE 1: ZERO BSS =====
    // MOVEA.L A5, A0 - start address calculation
    0x20, 0x4D,              // +0x04: 204D

    // SUBA.L #belowA5Size, A0 - a0 = A5 - belowA5Size [PATCH1]
    0x91, 0xFC,              // +0x06: 91FC
    0x00, 0x00, 0x00, 0x00,  // +0x08: [below-A5 size - PATCHED] (BELOW_A5_PATCH_OFFSET1 = 0x08)

    // MOVEQ #0, D0 - fill value
    0x70, 0x00,              // +0x0C: 7000

    // zeroLoop: (at +0x0E)
    // CMPA.L A5, A0 - compare a0 to A5
    0xB1, 0xCD,              // +0x0E: B1CD

    // BGE.S zeroDone (+4 bytes forward to +0x16)
    0x6C, 0x04,              // +0x10: 6C04

    // MOVE.B D0, (A0)+ - *a0++ = 0
    0x10, 0xC0,              // +0x12: 10C0

    // BRA.S zeroLoop (-8 bytes back to +0x0E)
    0x60, 0xF8,              // +0x14: 60F8

    // zeroDone: (at +0x16)
    // ===== PHASE 2: COPY DATA 0 =====
    // SUBQ.L #4, SP - space for GetResource return value
    0x59, 0x4F,              // +0x16: 594F

    // MOVE.L #'DATA', -(SP)
    0x2F, 0x3C,              // +0x18: 2F3C
    0x44, 0x41, 0x54, 0x41,  // +0x1A: 'DATA'

    // CLR.W -(SP) - resource ID 0
    0x42, 0x67,              // +0x1E: 4267

    // _GetResource trap
    0xA9, 0xA0,              // +0x20: A9A0

    // MOVEA.L (SP)+, A0 - pop handle
    0x20, 0x5F,              // +0x22: 205F

    // MOVE.L A0, D0 - test handle
    0x20, 0x08,              // +0x24: 2008

    // BEQ.S skipCopy (+38 bytes forward to skipCopy at +0x4E)
    0x67, 0x26,              // +0x26: 6726 (skip to +0x4E)

    // MOVE.L A0, -(SP) - push handle for HLock
    0x2F, 0x08,              // +0x28: 2F08

    // _HLock trap
    0xA0, 0x29,              // +0x2A: A029

    // ADDQ.L #4, SP - pop argument
    0x58, 0x4F,              // +0x2C: 584F

    // MOVEA.L (A0), A1 - dereference handle
    0x22, 0x50,              // +0x2E: 2250

    // MOVE.L (A1)+, D1 - get size (first 4 bytes)
    0x22, 0x19,              // +0x30: 2219

    // MOVEA.L A5, A2 - copy A5 to A2
    0x24, 0x4D,              // +0x32: 244D

    // SUBA.L #belowA5Size, A2 [PATCH2]
    0x95, 0xFC,              // +0x34: 95FC
    0x00, 0x00, 0x00, 0x00,  // +0x36: [below-A5 size - PATCHED] (BELOW_A5_PATCH_OFFSET2 = 0x36)

    // TST.L D1 - test if size is zero
    0x4A, 0x81,              // +0x3A: 4A81

    // BEQ.S skipLoop (+6 bytes forward to skipLoop at +0x44)
    0x67, 0x06,              // +0x3C: 6706 (skip to +0x44)

    // copyLoop: (at +0x3E)
    // MOVE.B (A1)+, (A2)+
    0x14, 0xD9,              // +0x3E: 14D9

    // SUBQ.L #1, D1
    0x53, 0x81,              // +0x40: 5381

    // BNE.S copyLoop (-6)
    0x66, 0xFA,              // +0x42: 66FA

    // skipLoop: (at +0x44)
    // MOVE.L A0, -(SP) - push handle for ReleaseResource
    0x2F, 0x08,              // +0x44: 2F08

    // _ReleaseResource trap (Pascal: pops its own 4-byte argument)
    0xA9, 0xA3,              // +0x46: A9A3

    // NOP - ReleaseResource already popped the argument (Pascal convention)
    0x4E, 0x71,              // +0x48: 4E71

    // NOP - GetResource return space was already consumed by MOVEA.L (SP)+
    0x4E, 0x71,              // +0x4A: 4E71

    // BRA.S toRestore (+2, skip the skipCopy landing)
    0x60, 0x02,              // +0x4C: 6002

    // skipCopy: (at +0x4E) - failure path lands here, falls through to toRestore
    // NOP - GetResource return space was already consumed by MOVEA.L (SP)+
    0x4E, 0x71,              // +0x4E: 4E71

    // toRestore: (at +0x50)
    // MOVEM.L (SP)+, D0-D2/A0-A2 - restore registers
    0x4C, 0xDF, 0x07, 0x07,  // +0x50: 4CDF 0707

    // BSR.W _main (displacement patched)
    0x61, 0x00,              // +0x54: 6100
    0x00, 0x00,              // +0x56: [displacement - PATCHED] (BSR_PATCH_OFFSET = 0x56)

    // MOVE.W D0, -(SP) - push return code
    0x3F, 0x00,              // +0x58: 3F00

    // _ExitToShell trap
    0xA9, 0xF4,              // +0x5A: A9F4
};

static constexpr size_t STARTUP_SIZE = sizeof(startupPrologue);
static constexpr size_t BELOW_A5_PATCH_OFFSET1 = 0x08; // BSS zeroing loop start address
static constexpr size_t BELOW_A5_PATCH_OFFSET2 = 0x36; // DATA copy destination
static constexpr size_t BSR_PATCH_OFFSET = 0x56;       // Offset to patch BSR displacement

// M68k ELF relocation types
enum {
  R_68K_NONE = 0,
  R_68K_32 = 1,        // 32-bit absolute
  R_68K_16 = 2,        // 16-bit absolute
  R_68K_8 = 3,         // 8-bit absolute
  R_68K_PC32 = 4,      // 32-bit PC-relative
  R_68K_PC16 = 5,      // 16-bit PC-relative
  R_68K_PC8 = 6,       // 8-bit PC-relative
};

bool Writer::link() {
  if (inputs.empty()) {
    errorMsg = "no input files";
    return false;
  }

  // === Phase 1: Build symbol table ===
  if (config->verbose) {
    errorHandler().outs() << "\n=== Phase 1: Building Symbol Table ===\n";
    errorHandler().outs() << "Processing " << inputs.size() << " input file(s)\n";
    errorHandler().outs().flush();
  }

  SymbolTable symtab;
  TrapDatabase trapDB;

  // Track user code base offsets for each input file
  std::vector<uint32_t> inputCodeOffsets;
  uint32_t currentCodeOffset = 0;

  // Track data offsets for each input file
  std::vector<uint32_t> inputDataOffsets;
  uint32_t currentDataOffset = 0;

  // First pass: collect all defined symbols and calculate offsets
  size_t inputIdx = 0;
  for (ELFReader *reader : inputs) {
    inputCodeOffsets.push_back(currentCodeOffset);
    inputDataOffsets.push_back(currentDataOffset);

    const ELFSection *text = reader->getTextSection();
    const ELFSection *data = reader->getDataSection();
    const ELFSection *rodata = reader->getRodataSection();
    const ELFSection *bss = reader->getBSSSection();

    // Build section index to type mapping for this reader
    size_t textIdx = reader->getTextSectionIndex();
    size_t dataIdx = 0, rodataIdx = 0, bssIdx = 0;

    // Find data, rodata, and bss section indices
    const auto &sections = reader->getSections();
    for (size_t i = 0; i < sections.size(); ++i) {
      if (sections[i].isData() && !sections[i].isBSS())
        dataIdx = i;
      else if (sections[i].isRodata())
        rodataIdx = i;
      else if (sections[i].isBSS())
        bssIdx = i;
    }

    if (config->verbose) {
      errorHandler().outs() << "\nInput[" << inputIdx << "]: (ELF object)\n";
      errorHandler().outs() << "  Code offset: 0x" << format_hex_no_prefix(currentCodeOffset, 4) << "\n";
      errorHandler().outs() << "  Data offset: 0x" << format_hex_no_prefix(currentDataOffset, 4) << "\n";
      if (text && !text->data.empty())
        errorHandler().outs() << "  .text size: " << text->data.size() << " bytes\n";
      if (data && !data->data.empty())
        errorHandler().outs() << "  .data size: " << data->data.size() << " bytes\n";
      if (rodata && !rodata->data.empty())
        errorHandler().outs() << "  .rodata size: " << rodata->data.size() << " bytes\n";
      if (bss && bss->size > 0)
        errorHandler().outs() << "  .bss size: " << bss->size << " bytes\n";
    }

    // Track rodata offset within data section
    size_t rodataOffset = currentDataOffset;
    if (data && !data->data.empty()) {
      rodataOffset += data->data.size();
    }

    // Process all global symbols
    size_t definedCount = 0;
    for (const auto &sym : reader->getSymbols()) {
      if (sym.isGlobal() && sym.sectionIdx != 0 && !sym.name.empty()) {
        if (sym.sectionIdx == textIdx) {
          // Code symbol
          uint32_t addr = currentCodeOffset + sym.value;
          symtab.addDefined(sym.name, addr);
          if (config->verbose) {
            errorHandler().outs() << "  Symbol: " << sym.name
                   << " @ 0x" << format_hex_no_prefix(addr, 4)
                   << " [code" << (sym.isFunction() ? "/func" : "") << "]\n";
          }
          definedCount++;
        } else if (sym.sectionIdx == dataIdx || sym.sectionIdx == bssIdx) {
          // Data/BSS symbol
          uint32_t addr = currentDataOffset + sym.value;
          symtab.addDataSymbol(sym.name, addr);
          if (config->verbose) {
            errorHandler().outs() << "  Symbol: " << sym.name
                   << " @ 0x" << format_hex_no_prefix(addr, 4)
                   << " [" << (sym.sectionIdx == bssIdx ? "bss" : "data") << "]\n";
          }
          definedCount++;
        } else if (sym.sectionIdx == rodataIdx) {
          // Read-only data symbol (placed after .data in the data section)
          uint32_t addr = rodataOffset + sym.value;
          symtab.addDataSymbol(sym.name, addr);
          if (config->verbose) {
            errorHandler().outs() << "  Symbol: " << sym.name
                   << " @ 0x" << format_hex_no_prefix(addr, 4)
                   << " [rodata]\n";
          }
          definedCount++;
        }
      }
    }

    // Update offsets for next input file
    if (text && !text->data.empty()) {
      currentCodeOffset += text->data.size();
    }
    if (data && !data->data.empty()) {
      currentDataOffset += data->data.size();
    }
    if (rodata && !rodata->data.empty()) {
      currentDataOffset += rodata->data.size();
    }
    if (bss) {
      currentDataOffset += bss->size;
    }

    // Add undefined symbols
    size_t undefinedCount = 0;
    for (const auto *sym : reader->getUndefinedSymbols()) {
      symtab.addUndefined(sym->name);
      undefinedCount++;
    }

    if (config->verbose) {
      errorHandler().outs() << "  Defined symbols: " << definedCount << "\n";
      if (undefinedCount > 0) {
        errorHandler().outs() << "  Undefined references: " << undefinedCount << "\n";
        for (const auto *sym : reader->getUndefinedSymbols()) {
          errorHandler().outs() << "    -> " << sym->name << "\n";
        }
      }
    }

    inputIdx++;
  }

  if (config->verbose) {
    errorHandler().outs() << "\nPhase 1 complete: "
           << symtab.getSymbols().size() << " defined symbols, "
           << currentCodeOffset << " bytes code, "
           << currentDataOffset << " bytes data\n";
  }

  // === Phase 2: Resolve undefined symbols against trap database ===
  if (!symtab.resolveTraps(trapDB)) {
    // Some symbols couldn't be resolved
    for (const auto &name : symtab.getUnresolvedSymbols()) {
      if (errorMsg.empty())
        errorMsg = "undefined symbol: " + name;
      else
        errorMsg += ", " + name;
    }
    return false;
  }

  // === Phase 3: Generate trap stubs ===
  // Only generate stubs for register-based traps (TRAP_REG_A0) like Delay.
  // Stack-based traps are emitted inline at the call site (see Phase 6).
  TrapStubs stubs;
  for (const auto *resolved : symtab.getTrapStubs()) {
    if (resolved->trapConv == TRAP_REG_A0) {
      // Register-based traps need a stub to move param from stack to A0
      stubs.addStub(resolved->name, resolved->trapWord, resolved->trapConv);
    }
    // Stack-based traps (TRAP_STACK, TRAP_DISPATCHER) are inlined
  }
  std::vector<uint8_t> stubCode = stubs.generate();

  // === Phase 4: Collect code and data ===
  std::vector<uint8_t> userCode;
  std::vector<uint8_t> allData;
  uint32_t bssSize = 0;

  // Collect function symbols for jump table
  std::vector<std::pair<std::string, uint32_t>> functions;

  for (ELFReader *reader : inputs) {
    const ELFSection *text = reader->getTextSection();
    if (text && !text->data.empty()) {
      // Record function offsets
      for (const auto &sym : reader->getSymbols()) {
        if (sym.isFunction() && sym.isGlobal()) {
          uint32_t offset = userCode.size() + sym.value;
          functions.push_back({sym.name, offset});
        }
      }
      userCode.insert(userCode.end(), text->data.begin(), text->data.end());
    }

    const ELFSection *data = reader->getDataSection();
    if (data && !data->data.empty()) {
      allData.insert(allData.end(), data->data.begin(), data->data.end());
    }

    // Include read-only data (.rodata) in the data section
    // This includes string literals that need A5-relative addressing
    const ELFSection *rodata = reader->getRodataSection();
    if (rodata && !rodata->data.empty()) {
      allData.insert(allData.end(), rodata->data.begin(), rodata->data.end());
    }

    const ELFSection *bss = reader->getBSSSection();
    if (bss) {
      bssSize += bss->size;
    }
  }

  // Find entry point
  const ELFSymbol *entrySym = nullptr;
  for (ELFReader *reader : inputs) {
    entrySym = reader->findSymbol(config->entrySymbol);
    if (entrySym)
      break;
  }

  if (!entrySym) {
    errorMsg = "entry point not found: " + config->entrySymbol;
    return false;
  }

  // Find main() offset in user code
  uint32_t mainOffsetInUserCode = 0;
  for (const auto &[name, offset] : functions) {
    if (name == config->entrySymbol) {
      mainOffsetInUserCode = offset;
      break;
    }
  }

  // Create data section early so we can compute A5-relative offsets
  DataSection dataSection;
  dataSection.setData(allData);
  dataSection.setBSSSize(bssSize);

  // === Phase 5: Build final code layout ===
  // Layout: [startup prologue] [trap stubs] [user code]
  std::vector<uint8_t> allCode;
  allCode.reserve(STARTUP_SIZE + stubCode.size() + userCode.size());

  // Copy startup prologue
  allCode.insert(allCode.end(), startupPrologue,
                 startupPrologue + STARTUP_SIZE);

  // Record where stubs start
  size_t stubsOffset = allCode.size();
  allCode.insert(allCode.end(), stubCode.begin(), stubCode.end());

  // Record where user code starts
  size_t userCodeOffset = allCode.size();
  allCode.insert(allCode.end(), userCode.begin(), userCode.end());

  // === Phase 6: Apply relocations ===
  if (config->verbose) {
    errorHandler().outs() << "\n=== Phase 6: Processing Relocations ===\n";
    errorHandler().outs() << "Code layout:\n";
    errorHandler().outs() << "  Startup prologue: 0x0000 - 0x"
           << format_hex_no_prefix(STARTUP_SIZE - 1, 4)
           << " (" << STARTUP_SIZE << " bytes)\n";
    if (!stubCode.empty()) {
      errorHandler().outs() << "  Trap stubs:       0x" << format_hex_no_prefix(stubsOffset, 4)
             << " - 0x" << format_hex_no_prefix(stubsOffset + stubCode.size() - 1, 4)
             << " (" << stubCode.size() << " bytes)\n";
    }
    errorHandler().outs() << "  User code:        0x" << format_hex_no_prefix(userCodeOffset, 4)
           << " - 0x" << format_hex_no_prefix(userCodeOffset + userCode.size() - 1, 4)
           << " (" << userCode.size() << " bytes)\n";
    errorHandler().outs() << "\nA5 World (below-A5 size = " << dataSection.getBelowA5Size() << " bytes):\n";
  }

  uint32_t codeBaseInFile = 0;  // Where this input's code starts in userCode
  size_t relocInputIdx = 0;
  size_t totalRelocCount = 0;

  for (ELFReader *reader : inputs) {
    const ELFSection *text = reader->getTextSection();
    if (!text || text->data.empty()) {
      relocInputIdx++;
      continue;
    }

    size_t textIdx = reader->getTextSectionIndex();
    const auto &relocs = reader->getRelocations(textIdx);

    if (config->verbose && !relocs.empty()) {
      errorHandler().outs() << "\nInput[" << relocInputIdx << "] relocations: "
             << relocs.size() << " entries\n";
      errorHandler().outs() << "  Code base in file: 0x" << format_hex_no_prefix(codeBaseInFile, 4) << "\n";
    }

    for (const auto &reloc : relocs) {
      const ELFSymbol *sym = reader->getSymbolByIndex(reloc.symbolIdx);
      if (!sym || sym->name.empty())
        continue;

      // Find the resolved symbol
      const ResolvedSymbol *resolved = symtab.find(sym->name);
      if (!resolved) {
        // Try with underscore stripped (C symbols)
        std::string stripped = sym->name;
        if (!stripped.empty() && stripped[0] == '_')
          stripped = stripped.substr(1);
        resolved = symtab.find(stripped);
      }

      if (!resolved) {
        // Symbol not found - might be a local symbol or section reference
        // For now, skip these
        if (config->verbose) {
          errorHandler().outs() << "  Reloc #" << totalRelocCount << " @ 0x"
                 << format_hex_no_prefix(reloc.offset, 4)
                 << " type=" << reloc.type
                 << " sym=" << sym->name
                 << " [SKIPPED - not found]\n";
        }
        continue;
      }

      totalRelocCount++;

      // Calculate target address
      // For code symbols: offset from start of allCode
      // For data symbols: A5-relative offset (negative, since data is below A5)
      uint32_t targetAddr;
      int32_t a5Offset = 0;  // Only used for data symbols
      bool isDataRef = false;

      if (resolved->isTrap) {
        // For register-based traps (TRAP_REG_A0), we need the stub address
        // For stack-based traps, we emit inline and don't need targetAddr
        if (resolved->trapConv == TRAP_REG_A0) {
          targetAddr = stubsOffset + stubs.getStubOffset(resolved->name);
        } else {
          targetAddr = 0;  // Not used for inline traps
        }
      } else if (resolved->isData) {
        // Data/BSS symbol - compute A5-relative offset
        // A5 points to the boundary between above-A5 and below-A5 areas
        // Data symbols are below A5, so they have negative offsets
        //
        // Memory layout below A5 (from bottom to top):
        //   [app data/rodata/padding] at bottom
        //   [QuickDraw globals (qd)] at top, just below A5
        //
        // QuickDraw globals MUST be at A5-206 to A5-1 (top of below-A5 area)
        // This is a Mac OS requirement - InitGraf expects this layout.
        //
        // Special case for "qd" symbol: force it to A5-206
        if (resolved->name == "qd") {
          // QD globals are 206 bytes, placed at top of below-A5
          // qd starts at A5 - 206
          a5Offset = -206;
        } else {
          // Other data symbols use standard layout from bottom
          uint32_t belowA5Size = dataSection.getBelowA5Size();
          a5Offset = -static_cast<int32_t>(belowA5Size - resolved->address);
        }
        isDataRef = true;
        targetAddr = 0;  // Not used for data symbols
      } else {
        // Code symbol
        targetAddr = userCodeOffset + resolved->address;
      }

      // Patch location in allCode
      uint32_t patchOffset = userCodeOffset + codeBaseInFile + reloc.offset;

      if (patchOffset >= allCode.size()) {
        errs() << "warning: relocation offset out of range for " << sym->name << "\n";
        continue;
      }

      // Verbose logging for each relocation
      if (config->verbose) {
        const char *typeStr = "unknown";
        switch (reloc.type) {
          case R_68K_32: typeStr = "R_68K_32"; break;
          case R_68K_16: typeStr = "R_68K_16"; break;
          case R_68K_8: typeStr = "R_68K_8"; break;
          case R_68K_PC32: typeStr = "R_68K_PC32"; break;
          case R_68K_PC16: typeStr = "R_68K_PC16"; break;
          case R_68K_PC8: typeStr = "R_68K_PC8"; break;
        }
        errorHandler().outs() << "  Reloc #" << totalRelocCount << " @ 0x"
               << format_hex_no_prefix(reloc.offset, 4)
               << " patch=0x" << format_hex_no_prefix(patchOffset, 4)
               << " " << typeStr
               << " sym=" << resolved->name;

        if (reloc.addend != 0) {
          errorHandler().outs() << " addend=" << reloc.addend;
        }

        if (resolved->isTrap) {
          errorHandler().outs() << " [TRAP 0x" << format_hex_no_prefix(resolved->trapWord, 4)
                 << " " << (resolved->trapConv == TRAP_REG_A0 ? "REG_A0" :
                            resolved->trapConv == TRAP_STACK ? "STACK" : "DISPATCHER");
          if (resolved->trapRetType != TRAP_RET_VOID) {
            errorHandler().outs() << " ret=" << (resolved->trapRetType == TRAP_RET_POINTER ? "ptr" :
                                  resolved->trapRetType == TRAP_RET_WORD ? "word" : "byte");
          }
          errorHandler().outs() << "]";
        } else if (isDataRef) {
          errorHandler().outs() << " [DATA A5" << (a5Offset >= 0 ? "+" : "") << a5Offset << "]";
        } else {
          errorHandler().outs() << " [CODE @ 0x" << format_hex_no_prefix(targetAddr, 4) << "]";
        }
        errorHandler().outs() << "\n";
      }

      switch (reloc.type) {
        case R_68K_32: {
          // Absolute 32-bit relocation
          // Check if this is a JSR abs.L (0x4EB9)
          if (patchOffset >= 2 &&
              allCode[patchOffset - 2] == 0x4E &&
              allCode[patchOffset - 1] == 0xB9) {

            if (resolved->isTrap && resolved->trapConv != TRAP_REG_A0) {
              // INLINE TRAP: Replace JSR abs.L (6 bytes) with trap + padding
              // JSR abs.L: 4EB9 XXXXXXXX (6 bytes)
              //
              // For traps that DON'T return values:
              //   Replace: trap_word NOP NOP (6 bytes total)
              //
              // For traps that RETURN values (Pascal calling convention):
              //   Replace: trap_word MOVEA.L (SP)+,A0 NOP (6 bytes total)
              //   The trap pushes the return value onto the stack.
              //   We pop it into A0, where C code expects pointer returns.
              //   The C compiler then generates MOVE.L A0,D0 to get it into D0.
              //
              // This matches how CodeWarrior emits traps - inline, no stub.
              uint16_t trapWord = resolved->trapWord;
              allCode[patchOffset - 2] = (trapWord >> 8) & 0xFF;
              allCode[patchOffset - 1] = trapWord & 0xFF;

              // Handle different return types
              // NOTE: Mac Toolbox traps do NOT auto-pop parameters (bit 10 clear).
              // The caller is responsible for stack cleanup. Since the clang m68k
              // backend writes params to (SP) without allocating, and traps don't
              // pop, SP remains stable after the trap. We just need NOPs for padding.
              switch (resolved->trapRetType) {
                case TRAP_RET_POINTER:
                  // MOVEA.L (SP)+, A0 = 0x205F
                  // Pop 4-byte pointer return value from stack into A0
                  allCode[patchOffset + 0] = 0x20;
                  allCode[patchOffset + 1] = 0x5F;
                  allCode[patchOffset + 2] = 0x4E;  // NOP
                  allCode[patchOffset + 3] = 0x71;
                  break;
                case TRAP_RET_BYTE:
                  // MOVE.B (SP)+, D0 = 0x101F
                  // Pop 1-byte Boolean return value from stack into D0
                  allCode[patchOffset + 0] = 0x10;
                  allCode[patchOffset + 1] = 0x1F;
                  allCode[patchOffset + 2] = 0x4E;  // NOP
                  allCode[patchOffset + 3] = 0x71;
                  break;
                case TRAP_RET_WORD:
                  // MOVE.W (SP)+, D0 = 0x301F
                  // Pop 2-byte short return value from stack into D0
                  allCode[patchOffset + 0] = 0x30;
                  allCode[patchOffset + 1] = 0x1F;
                  allCode[patchOffset + 2] = 0x4E;  // NOP
                  allCode[patchOffset + 3] = 0x71;
                  break;
                default:  // TRAP_RET_VOID
                  allCode[patchOffset + 0] = 0x4E;  // NOP
                  allCode[patchOffset + 1] = 0x71;
                  allCode[patchOffset + 2] = 0x4E;  // NOP
                  allCode[patchOffset + 3] = 0x71;
                  break;
              }

              if (config->verbose) {
                errorHandler().outs() << "  Inline trap: " << resolved->name
                       << " -> 0x" << format_hex_no_prefix(trapWord, 4);
                if (resolved->trapRetType != TRAP_RET_VOID)
                  errorHandler().outs() << " (returns "
                         << (resolved->trapRetType == TRAP_RET_POINTER ? "ptr" :
                             resolved->trapRetType == TRAP_RET_BYTE ? "byte" : "word")
                         << ")";
                errorHandler().outs() << "\n";
              }
            } else {
              // Non-trap or register-based trap: use BSR to stub
              // Convert JSR abs.L to BSR.W (PC-relative)
              // JSR abs.L: 4EB9 XXXXXXXX (6 bytes)
              // BSR.W:     6100 XXXX (4 bytes) + 2 bytes NOP padding
              //
              // Calculate PC-relative displacement
              // PC after reading BSR.W opcode = patchOffset - 2 + 2 = patchOffset
              // But BSR.W displacement is from PC after opcode word
              // So displacement = targetAddr - patchOffset
              int32_t displacement = static_cast<int32_t>(targetAddr) -
                                     static_cast<int32_t>(patchOffset) +
                                     reloc.addend;

              if (displacement >= -32768 && displacement <= 32767) {
                // Use BSR.W (4 bytes total, need 2 bytes NOP padding)
                allCode[patchOffset - 2] = 0x61;  // BSR.W opcode high
                allCode[patchOffset - 1] = 0x00;  // BSR.W opcode low
                allCode[patchOffset + 0] = (displacement >> 8) & 0xFF;
                allCode[patchOffset + 1] = displacement & 0xFF;
                allCode[patchOffset + 2] = 0x4E;  // NOP
                allCode[patchOffset + 3] = 0x71;  // NOP

                if (config->verbose) {
                  errorHandler().outs() << "    -> JSR abs.L -> BSR.W (disp=" << displacement
                         << ", target=0x" << format_hex_no_prefix(targetAddr, 4) << ")\n";
                }
              } else {
                // Use BSR.L for long displacement (0x61FF)
                allCode[patchOffset - 2] = 0x61;  // BSR.L opcode high
                allCode[patchOffset - 1] = 0xFF;  // BSR.L opcode low
                allCode[patchOffset + 0] = (displacement >> 24) & 0xFF;
                allCode[patchOffset + 1] = (displacement >> 16) & 0xFF;
                allCode[patchOffset + 2] = (displacement >> 8) & 0xFF;
                allCode[patchOffset + 3] = displacement & 0xFF;

                if (config->verbose) {
                  errorHandler().outs() << "    -> JSR abs.L -> BSR.L (disp=" << displacement
                         << ", target=0x" << format_hex_no_prefix(targetAddr, 4) << ")\n";
                }
              }
            }
          } else {
            // Not a JSR - regular absolute relocation
            int32_t value;
            if (isDataRef) {
              // Data symbol - use A5-relative offset (negative)
              // The generated code does: A5 + offset = effective address
              // So we store the signed A5-relative offset
              value = a5Offset + reloc.addend;
              if (config->verbose) {
                errorHandler().outs() << "    -> A5-relative patch: value=" << value
                       << " (A5" << (value >= 0 ? "+" : "") << value << ")\n";
              }
            } else {
              // Code symbol - compute offset from CODE 1 start
              value = CodeSegment::HEADER_SIZE + targetAddr + reloc.addend;
              if (config->verbose) {
                errorHandler().outs() << "    -> Absolute code ref: value=0x"
                       << format_hex_no_prefix(value, 8) << "\n";
              }
            }
            if (patchOffset + 4 > allCode.size()) break;
            allCode[patchOffset + 0] = (value >> 24) & 0xFF;
            allCode[patchOffset + 1] = (value >> 16) & 0xFF;
            allCode[patchOffset + 2] = (value >> 8) & 0xFF;
            allCode[patchOffset + 3] = value & 0xFF;
          }
          break;
        }

        case R_68K_PC32: {
          // PC-relative 32-bit (BSR.L, BRA.L)
          // Check if this is a BRA.L (0x60FF) to a stack-based trap
          // BRA.L: 60FF XXXXXXXX (6 bytes) - used for tail calls
          if (patchOffset >= 2 &&
              allCode[patchOffset - 2] == 0x60 &&
              allCode[patchOffset - 1] == 0xFF &&
              resolved->isTrap && resolved->trapConv != TRAP_REG_A0) {
            // INLINE TRAP: Replace BRA.L (6 bytes) with trap + padding
            // For tail calls (BRA), we don't need to handle return values
            // because control never returns to this point
            uint16_t trapWord = resolved->trapWord;
            allCode[patchOffset - 2] = (trapWord >> 8) & 0xFF;
            allCode[patchOffset - 1] = trapWord & 0xFF;

            // Fill remaining 4 bytes with NOPs (these are never executed for
            // noreturn functions like ExitToShell, but maintain code size)
            allCode[patchOffset + 0] = 0x4E;  // NOP
            allCode[patchOffset + 1] = 0x71;
            allCode[patchOffset + 2] = 0x4E;  // NOP
            allCode[patchOffset + 3] = 0x71;

            if (config->verbose) {
              errorHandler().outs() << "  Inline trap (tail call): " << resolved->name
                     << " -> 0x" << format_hex_no_prefix(trapWord, 4) << "\n";
            }
          } else if (patchOffset >= 2 &&
                     allCode[patchOffset - 2] == 0x61 &&
                     allCode[patchOffset - 1] == 0xFF &&
                     resolved->isTrap && resolved->trapConv != TRAP_REG_A0) {
            // BSR.L (0x61FF) to a stack-based trap - inline it
            uint16_t trapWord = resolved->trapWord;
            allCode[patchOffset - 2] = (trapWord >> 8) & 0xFF;
            allCode[patchOffset - 1] = trapWord & 0xFF;

            // Handle return values for BSR (non-tail call)
            // NOTE: Mac Toolbox traps do NOT auto-pop parameters.
            switch (resolved->trapRetType) {
              case TRAP_RET_POINTER:
                allCode[patchOffset + 0] = 0x20;  // MOVEA.L (SP)+, A0
                allCode[patchOffset + 1] = 0x5F;
                allCode[patchOffset + 2] = 0x4E;  // NOP
                allCode[patchOffset + 3] = 0x71;
                break;
              case TRAP_RET_BYTE:
                allCode[patchOffset + 0] = 0x10;  // MOVE.B (SP)+, D0
                allCode[patchOffset + 1] = 0x1F;
                allCode[patchOffset + 2] = 0x4E;  // NOP
                allCode[patchOffset + 3] = 0x71;
                break;
              case TRAP_RET_WORD:
                allCode[patchOffset + 0] = 0x30;  // MOVE.W (SP)+, D0
                allCode[patchOffset + 1] = 0x1F;
                allCode[patchOffset + 2] = 0x4E;  // NOP
                allCode[patchOffset + 3] = 0x71;
                break;
              default:  // TRAP_RET_VOID
                allCode[patchOffset + 0] = 0x4E;  // NOP
                allCode[patchOffset + 1] = 0x71;
                allCode[patchOffset + 2] = 0x4E;  // NOP
                allCode[patchOffset + 3] = 0x71;
                break;
            }

            if (config->verbose) {
              errorHandler().outs() << "  Inline trap (call): " << resolved->name
                     << " -> 0x" << format_hex_no_prefix(trapWord, 4);
              if (resolved->trapRetType != TRAP_RET_VOID)
                errorHandler().outs() << " (returns "
                       << (resolved->trapRetType == TRAP_RET_POINTER ? "ptr" :
                           resolved->trapRetType == TRAP_RET_BYTE ? "byte" : "word")
                       << ")";
              errorHandler().outs() << "\n";
            }
          } else {
            // Regular PC-relative relocation
            // PC points to instruction + 2 after fetching opcode
            int32_t displacement = static_cast<int32_t>(targetAddr) -
                                   static_cast<int32_t>(patchOffset) +
                                   reloc.addend;
            if (patchOffset + 4 > allCode.size()) break;
            allCode[patchOffset + 0] = (displacement >> 24) & 0xFF;
            allCode[patchOffset + 1] = (displacement >> 16) & 0xFF;
            allCode[patchOffset + 2] = (displacement >> 8) & 0xFF;
            allCode[patchOffset + 3] = displacement & 0xFF;

            if (config->verbose && !resolved->isTrap) {
              errorHandler().outs() << "    -> PC-relative: disp=" << displacement
                     << " target=0x" << format_hex_no_prefix(targetAddr, 4) << "\n";
            }
          }
          break;
        }

        case R_68K_PC16: {
          // PC-relative 16-bit (BSR.W, BRA.W)
          int32_t displacement = static_cast<int32_t>(targetAddr) -
                                 static_cast<int32_t>(patchOffset) +
                                 reloc.addend;
          if (displacement < -32768 || displacement > 32767) {
            errs() << "warning: PC-relative displacement out of range for "
                   << sym->name << " (" << displacement << ")\n";
            break;
          }
          if (patchOffset + 2 > allCode.size()) break;
          allCode[patchOffset + 0] = (displacement >> 8) & 0xFF;
          allCode[patchOffset + 1] = displacement & 0xFF;
          break;
        }

        case R_68K_16: {
          // Absolute 16-bit
          uint32_t value = targetAddr + reloc.addend;
          if (value > 65535) {
            errs() << "warning: 16-bit absolute overflow for " << sym->name << "\n";
            break;
          }
          if (patchOffset + 2 > allCode.size()) break;
          allCode[patchOffset + 0] = (value >> 8) & 0xFF;
          allCode[patchOffset + 1] = value & 0xFF;
          break;
        }

        default:
          // Unsupported relocation type
          if (config->verbose) {
            errs() << "warning: unsupported relocation type " << reloc.type
                   << " for " << sym->name << "\n";
          }
          break;
      }
    }

    codeBaseInFile += text->data.size();
    relocInputIdx++;
  }

  if (config->verbose) {
    errorHandler().outs() << "\nPhase 6 complete: " << totalRelocCount << " relocations processed\n";
  }

  // === Phase 7: Patch startup prologue ===
  if (config->verbose) {
    errorHandler().outs() << "\n=== Phase 7: Patching Startup Prologue ===\n";
  }

  // 7a. Patch the below-A5 size in both SUBA.L instructions
  // PATCH1: BSS zeroing loop start address calculation
  // PATCH2: DATA copy destination calculation
  uint32_t belowA5Size = dataSection.getBelowA5Size();

  // Patch location 1 (BSS zeroing)
  allCode[BELOW_A5_PATCH_OFFSET1 + 0] = (belowA5Size >> 24) & 0xFF;
  allCode[BELOW_A5_PATCH_OFFSET1 + 1] = (belowA5Size >> 16) & 0xFF;
  allCode[BELOW_A5_PATCH_OFFSET1 + 2] = (belowA5Size >> 8) & 0xFF;
  allCode[BELOW_A5_PATCH_OFFSET1 + 3] = belowA5Size & 0xFF;

  // Patch location 2 (DATA copy)
  allCode[BELOW_A5_PATCH_OFFSET2 + 0] = (belowA5Size >> 24) & 0xFF;
  allCode[BELOW_A5_PATCH_OFFSET2 + 1] = (belowA5Size >> 16) & 0xFF;
  allCode[BELOW_A5_PATCH_OFFSET2 + 2] = (belowA5Size >> 8) & 0xFF;
  allCode[BELOW_A5_PATCH_OFFSET2 + 3] = belowA5Size & 0xFF;

  if (config->verbose) {
    errorHandler().outs() << "  Below-A5 size: " << belowA5Size << " bytes (0x"
           << format_hex_no_prefix(belowA5Size, 8)
           << ") @ offsets 0x" << format_hex_no_prefix(BELOW_A5_PATCH_OFFSET1, 4)
           << " and 0x" << format_hex_no_prefix(BELOW_A5_PATCH_OFFSET2, 4) << "\n";
  }

  // 7b. Patch BSR.W displacement to main()
  // main() is at: userCodeOffset + mainOffsetInUserCode
  // BSR.W is at offset BSR_PATCH_OFFSET-2, displacement at BSR_PATCH_OFFSET
  // PC after BSR.W opcode = BSR_PATCH_OFFSET
  uint32_t mainOffset = userCodeOffset + mainOffsetInUserCode;
  int16_t displacement = static_cast<int16_t>(mainOffset - BSR_PATCH_OFFSET);

  if (mainOffset > 32767 + BSR_PATCH_OFFSET) {
    errorMsg = "main() is too far from startup code for BSR.W (>32KB)";
    return false;
  }

  allCode[BSR_PATCH_OFFSET + 0] = (displacement >> 8) & 0xFF;
  allCode[BSR_PATCH_OFFSET + 1] = displacement & 0xFF;

  if (config->verbose) {
    errorHandler().outs() << "  Entry point: " << config->entrySymbol << " @ 0x"
           << format_hex_no_prefix(mainOffset, 4) << "\n";
    errorHandler().outs() << "  BSR displacement: " << displacement
           << " (0x" << format_hex_no_prefix(static_cast<uint16_t>(displacement), 4)
           << ") @ offset 0x" << format_hex_no_prefix(BSR_PATCH_OFFSET, 4) << "\n";
    errorHandler().outs() << "Phase 7 complete\n";
  }

  // === Phase 8: Build jump table and resources ===
  JumpTable jt;

  // Add __start as first jump table entry
  jt.addEntry("__start", 1, CodeSegment::HEADER_SIZE);

  // Add other global functions (with adjusted offsets)
  for (const auto &[name, offset] : functions) {
    if (name != config->entrySymbol) {
      uint32_t adjustedOffset =
          CodeSegment::HEADER_SIZE + userCodeOffset + offset;
      jt.addEntry(name, 1, adjustedOffset);
    }
  }

  // Set below-A5 size (dataSection was created earlier for A5-relative offsets)
  jt.setBelowA5Size(dataSection.getBelowA5Size());

  // Create code segment
  CodeSegment codeSeg(1, config->segmentName);
  codeSeg.setCode(allCode);

  // Create SIZE resource
  SizeResource sizeRes;
  sizeRes.setFlags(config->sizeFlags);
  sizeRes.setPreferredSize(config->heapSize);
  sizeRes.setMinimumSize(config->minHeapSize);

  // Generate resource data
  auto code0Data = jt.generate();
  auto code1Data = codeSeg.generate(0, jt.size());
  auto data0Data = dataSection.generate();
  auto sizeData = sizeRes.generate();

  // Create resource writer
  ResourceWriter rsrc;
  rsrc.addResource("CODE", 0, code0Data, resPurgeable | resProtected);
  rsrc.addResource("CODE", 1, code1Data, resPurgeable | resProtected,
                   config->segmentName);
  if (!data0Data.empty()) {
    rsrc.addResource("DATA", 0, data0Data, resPurgeable | resProtected);
  }
  rsrc.addResource("SIZE", -1, sizeData, 0);

  // Create empty data fork file
  std::error_code ec;
  {
    raw_fd_ostream dataFork(outputPath, ec, sys::fs::OF_None);
    if (ec) {
      errorMsg = "cannot create output file: " + outputPath;
      return false;
    }
  }

  // Write resource fork
  bool rsrcWritten = rsrc.writeToFile(outputPath);
  std::string rsrcFilePath = outputPath + ".rsrc";
  rsrc.writeToRsrcFile(rsrcFilePath);

  if (!rsrcWritten && config->verbose) {
    errorHandler().outs() << "Note: Could not write resource fork directly.\n";
    errorHandler().outs() << "Resource data written to: " << rsrcFilePath << "\n";
  }

  // Set file type to APPL
  std::string creator = config->creatorCode;
  if (creator.size() != 4)
    creator = "????";

  char finderInfo[65];
  snprintf(finderInfo, sizeof(finderInfo),
           "4150504C%02X%02X%02X%02X0000000000000000"
           "0000000000000000",
           (uint8_t)creator[0], (uint8_t)creator[1],
           (uint8_t)creator[2], (uint8_t)creator[3]);

  std::string setFileCmd = "/usr/bin/SetFile -t APPL -c '" + creator + "' \"" +
                           outputPath + "\" 2>/dev/null";
  if (system(setFileCmd.c_str()) != 0) {
    std::string xattrCmd = "xattr -wx com.apple.FinderInfo '" +
                           std::string(finderInfo) + "' \"" +
                           outputPath + "\" 2>/dev/null";
    system(xattrCmd.c_str());
  }

  if (config->verbose) {
    // Count inline vs stub traps
    size_t inlineTraps = 0, stubTraps = 0;
    for (const auto *resolved : symtab.getTrapStubs()) {
      if (resolved->trapConv == TRAP_REG_A0)
        stubTraps++;
      else
        inlineTraps++;
    }

    errorHandler().outs() << "\n=== Phase 8: Resource Generation ===\n";
    errorHandler().outs() << "Generated classic 68K application:\n";
    errorHandler().outs() << "  CODE 0: " << code0Data.size() << " bytes (jump table)\n";
    errorHandler().outs() << "  CODE 1: " << code1Data.size() << " bytes (code)\n";
    errorHandler().outs() << "    - Startup CRT: " << STARTUP_SIZE << " bytes\n";
    if (stubCode.size() > 0)
      errorHandler().outs() << "    - Trap stubs: " << stubCode.size() << " bytes ("
             << stubTraps << " register-based traps)\n";
    if (inlineTraps > 0)
      errorHandler().outs() << "    - Inline traps: " << inlineTraps << " stack-based traps\n";
    errorHandler().outs() << "    - User code: " << userCode.size() << " bytes\n";
    if (!data0Data.empty())
      errorHandler().outs() << "  DATA 0: " << data0Data.size() << " bytes (globals)\n";
    errorHandler().outs() << "  SIZE -1: " << sizeData.size() << " bytes\n";
    errorHandler().outs() << "  Entry point: __start -> " << config->entrySymbol << "\n";

    // Detailed memory layout
    errorHandler().outs() << "\n=== Memory Layout ===\n";
    errorHandler().outs() << "CODE 1 (offset from CODE 1 start):\n";
    errorHandler().outs() << "  0x0000-0x" << format_hex_no_prefix(STARTUP_SIZE - 1, 4)
           << ": Startup prologue (" << STARTUP_SIZE << " bytes)\n";
    if (!stubCode.empty()) {
      errorHandler().outs() << "  0x" << format_hex_no_prefix(stubsOffset, 4) << "-0x"
             << format_hex_no_prefix(stubsOffset + stubCode.size() - 1, 4)
             << ": Trap stubs (" << stubCode.size() << " bytes, "
             << stubTraps << " stubs)\n";
    }
    errorHandler().outs() << "  0x" << format_hex_no_prefix(userCodeOffset, 4) << "-0x"
           << format_hex_no_prefix(userCodeOffset + userCode.size() - 1, 4)
           << ": User code (" << userCode.size() << " bytes)\n";

    errorHandler().outs() << "\nA5 World:\n";
    errorHandler().outs() << "  A5-" << belowA5Size << " to A5-1: Globals ("
           << belowA5Size << " bytes below A5)\n";
    errorHandler().outs() << "  A5+0 to A5+" << jt.size()
           << ": Jump table (" << jt.size() << " bytes above A5)\n";

    errorHandler().outs() << "\n=== Linking Complete ===\n";
    errorHandler().outs() << "Output: " << outputPath << "\n";
    errorHandler().outs().flush();
  }

  return true;
}

} // namespace lld::classic68k
