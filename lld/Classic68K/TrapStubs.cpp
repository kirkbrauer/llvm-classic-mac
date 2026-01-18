//===- TrapStubs.cpp - Mac Toolbox A-Trap stub generation -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "TrapStubs.h"
#include "Config.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace lld::classic68k {

void TrapStubs::addStub(llvm::StringRef name, uint16_t trapWord,
                        TrapCallingConv conv) {
  Stub s;
  s.name = name.str();
  s.trapWord = trapWord;
  s.conv = conv;
  stubs.push_back(s);
  cacheValid = false;
}

std::vector<uint8_t> TrapStubs::generate() const {
  std::vector<uint8_t> code;

  if (config && config->verbose && !stubs.empty()) {
    outs() << "\n=== Phase 3: Generating Trap Stubs ===\n";
    outs() << "Total stubs needed: " << stubs.size() << "\n";
  }

  size_t offset = 0;
  for (const auto &stub : stubs) {
    if (config && config->verbose) {
      size_t stubSize = (stub.conv == TRAP_REG_A0) ? 8 : 16;
      outs() << "  Stub: " << stub.name
             << " @ offset 0x" << format_hex_no_prefix(offset, 4)
             << " (trap=0x" << format_hex_no_prefix(stub.trapWord, 4) << ")"
             << " - " << stubSize << " bytes ("
             << (stub.conv == TRAP_REG_A0 ? "REG_A0" :
                 stub.conv == TRAP_STACK ? "STACK" : "DISPATCHER")
             << ")\n";
      offset += stubSize;
    }
    switch (stub.conv) {
      case TRAP_REG_A0:
        // Register A0 calling convention (Delay, NewPtr, etc.)
        // C caller pushes parameter on stack, we need to move it to A0
        //
        // Stack on entry (after BSR/JSR):
        //   SP+0: return address (4 bytes)
        //   SP+4: parameter (4 bytes for long, 2 for word)
        //
        // For Delay: parameter is unsigned long (4 bytes)
        //   MOVE.L 4(SP), A0    ; 206F 0004 - load param into A0
        //   trap_word           ; AXXX
        //   RTS                 ; 4E75
        //
        // Total: 8 bytes
        code.push_back(0x20);  // MOVE.L
        code.push_back(0x6F);  // d(SP), A0
        code.push_back(0x00);  // offset high
        code.push_back(0x04);  // offset low (4 bytes past return addr)
        code.push_back((stub.trapWord >> 8) & 0xFF);
        code.push_back(stub.trapWord & 0xFF);
        code.push_back(0x4E);  // RTS
        code.push_back(0x75);
        break;

      case TRAP_STACK:
      case TRAP_DISPATCHER:
        // Stack-based calling convention (most Toolbox traps)
        //
        // Problem: C calling convention vs Pascal calling convention.
        //
        // C compiler generates:
        //   SUBA.L #N, SP        ; Allocate stack frame
        //   MOVE.L param, (SP)   ; Write param to pre-allocated slot
        //   JSR TrapName         ; Call (pushes return addr)
        //   ; Continues, expects SP same as before JSR after return
        //
        // But Toolbox traps use Pascal convention:
        //   - Parameters at SP+0 (after return addr is removed)
        //   - Trap POPS its parameters before returning
        //
        // Stack on entry (after JSR from C code, SP = X):
        //   X+0: return address (4 bytes)
        //   X+4: first parameter
        //   X+8: second parameter (if any)
        //   ...
        //
        // After RTS normally: SP = X (retaddr popped)
        // C caller expects: SP = X
        //
        // Solution: Save return address and entry SP using a mini stack frame.
        //
        // IMPORTANT: Traps may clobber A0, A1, D0, D1, D2. We must save
        // critical values somewhere they won't be touched. D3-D7 and A2-A6
        // are callee-saved, so we must preserve them if we use them.
        //
        // Safe approach using callee-saved registers with save/restore:
        //   MOVE.L  A2, -(SP)   ; 2F0A - Save A2 (preserved register)
        //   MOVEA.L 4(SP), A2   ; 246F 0004 - A2 = return address
        //   LEA     8(SP), SP   ; 4FEF 0008 - Pop return addr + saved A2
        //   trap_word           ; AXXX - Trap runs, pops params
        //   MOVE.L  A2, -(SP)   ; 2F0A - Push return address
        //   MOVEA.L 4(SP), A2   ; 246F 0004 - Restore A2 (original saved value after trap stack change)
        //
        // Wait, this is getting complicated. Let's use a simpler approach:
        // Save A2 to stack, use it for return address, execute trap, restore.
        //
        // Actually the simplest safe approach:
        //   LINK    A6, #0      ; 4E56 0000 - Create stack frame (saves A6, A6=SP)
        //   MOVE.L  4(A6), A0   ; 206E 0004 - A0 = return address (from before LINK)
        //                       ; Actually after LINK: 0(A6)=saved A6, 4(A6)=ret addr, 8(A6)=params
        //   ... this is getting too complex for reliable stub generation.
        //
        // SIMPLEST: Use the stack as scratch space BELOW where trap will touch.
        // Since trap pops params from above current SP, we can use negative offsets.
        //
        // Actually, let's just do what a real compiler would do:
        //   MOVE.L (SP)+, -(A7) won't work...
        //
        // FINAL APPROACH: Use the fact that after the trap, SP has moved.
        // We save the return address to A2 (callee-saved), save A2 first.
        //
        //   MOVEM.L A2, -(SP)   ; 48E7 0008 - Save A2
        //   MOVEA.L 4(SP), A2   ; 246F 0004 - A2 = return address
        //   ADDQ.L  #8, SP      ; 5088 - Pop saved A2 + return address
        //   trap_word           ; AXXX - Trap runs, pops params
        //   SUBA.L  #8, SP      ; 91FC 0000 0008 - Make room for A2 + ret addr
        //   MOVE.L  A2, 4(SP)   ; 2F4A 0004 - Store return address
        //   MOVEM.L (SP)+, A2   ; 4CDF 0004 - Restore A2
        //   RTS                 ; 4E75
        //
        // Hmm, that doesn't work because the trap changed SP by popping params.
        //
        // Let's think about this differently. The simplest reliable approach:
        // Just inline the trap with proper setup - no function call overhead.
        //
        // For now, let's use the approach that works: Save to A2 (callee-saved),
        // but also track where the stack was.
        //
        // WORKING APPROACH: Save return address below the stack frame.
        //   MOVE.L  (SP)+, -4(SP)  ; Move ret addr down 4 bytes
        //   ... no that's illegal on 68K
        //
        // OK, truly simplest working approach - use LINK/UNLK:
        //   LINK    A6, #0         ; 4E56 0000 - A6 = old SP, push old A6
        //   MOVE.L  8(A6), -(SP)   ; Copy first param to stack (trap expects params at SP)
        //   ... this gets complex for variable param counts
        //
        // PRAGMATIC APPROACH: Just execute trap inline like initgraf_inline does.
        // The "error type 3" was because our stub was wrong. Let's try the
        // simple inline approach but still as a callable stub:
        //
        //   The trap expects: SP -> param1, param2, ...
        //   We have: SP -> return_addr, param1, param2, ...
        //
        //   Simple fix: swap return address with params using a temp register
        //   that won't be clobbered. Actually A2-A4 are callee-saved in Mac OS.
        //
        // Let me try the MOVEM approach properly:
        //   MOVEM.L D3/A2, -(SP)    ; 48E7 1008 - Save D3 and A2 (8 bytes)
        //   MOVE.L  8(SP), D3       ; 262F 0008 - D3 = return address
        //   MOVEA.L SP, A2          ; 244F - A2 = current SP (with saved regs)
        //   LEA     12(SP), SP      ; 4FEF 000C - Pop saved regs + ret addr
        //   trap_word               ; AXXX - Trap pops params
        //   MOVEA.L A2, SP          ; 2E4A - Restore SP to after saved regs
        //   MOVE.L  D3, 8(SP)       ; 2F43 0008 - Put return addr back
        //   MOVEM.L (SP)+, D3/A2    ; 4CDF 1008 - Restore D3 and A2
        //   RTS                     ; 4E75
        //
        // This is 22 bytes. Let's use a shorter approach.
        //
        // Actually, I realize the issue: when we restore SP to A2, we're
        // pointing to where saved D3/A2 were. But the trap popped params,
        // so the stack is in a weird state.
        //
        // THE REAL SOLUTION: We need to track both entry SP and the saved regs.
        // Or simpler: just use the stack frame pointer approach (A6/LINK).
        //
        // LINK/UNLK approach (most robust):
        //   LINK    A6, #-4        ; 4E56 FFFC - Create frame, allocate 4 bytes
        //   MOVE.L  4(A6), -4(A6)  ; Save return address to local (-4(A6))
        //   LEA     8(A6), SP      ; Point SP at params (skip ret addr + saved A6)
        //   trap_word              ; Trap pops params from SP
        //   MOVEA.L A6, SP         ; Restore frame pointer
        //   MOVE.L  -4(A6), 4(A6)  ; Restore return address
        //   UNLK    A6             ; 4E5E
        //   RTS                    ; 4E75
        //
        // Actually, this STILL won't work because after the trap pops params,
        // the memory at 4(A6) and 8(A6) may have been popped/changed.
        //
        // THE ACTUAL ISSUE: Pascal calling convention has the CALLER clean
        // up the return address, not the callee. The trap doesn't push a
        // return address - it just pops params and returns to where it
        // was called from (the A-trap dispatcher handles this).
        //
        // So our stub approach fundamentally won't work because:
        // 1. C code calls our stub with BSR/JSR (pushes return to stub)
        // 2. Our stub should call trap (which pops its params)
        // 3. Trap returns... to where? It expects return addr on stack!
        //
        // Wait, A-line traps don't use RTS. They're exceptions that return
        // via the trap dispatcher. The trap word is like an inline syscall.
        // After the trap instruction, execution continues at the next
        // instruction.
        //
        // So the flow is:
        // 1. Code executes AXXX trap word
        // 2. 68K generates A-line exception, jumps to trap dispatcher
        // 3. Trap dispatcher identifies trap, calls appropriate handler
        // 4. Handler pops params from stack, does work
        // 5. Handler finishes, trap dispatcher returns to instruction AFTER trap
        //
        // So our stub should be:
        //   [entry: SP -> ret_to_caller, params...]
        //   MOVE.L  (SP)+, A2      ; Pop return-to-caller into A2
        //   trap_word              ; AXXX - executes, pops params, continues here
        //   JMP     (A2)           ; Return to caller
        //
        // The issue with our previous attempt was that we used A0/A1 which
        // get clobbered. A2 is callee-saved so should survive the trap.
        //
        // BUT WAIT: We need to SAVE A2 first since it's callee-saved!
        //
        // CORRECT STUB:
        //   MOVE.L  A2, -(SP)      ; 2F0A - Save A2
        //   MOVE.L  4(SP), A2      ; 246F 0004 - A2 = return address
        //   ADDQ.L  #8, SP         ; 508F - Pop saved A2 and return address
        //   trap_word              ; AXXX - Trap pops params, SP moves
        //   MOVE.L  A2, -(SP)      ; 2F0A - Push return address back
        //   MOVEA.L saved_A2, A2   ; Restore A2... but where is it?!
        //
        // The problem: after trap pops params, we don't know where saved A2 is.
        //
        // SOLUTION: Save both A2 and entry SP to A2-relative locations, OR
        // use two saved registers:
        //
        //   MOVEM.L D3/A2, -(SP)   ; 48E7 1008 - Save D3, A2 (8 bytes)
        //   MOVE.L  8(SP), D3      ; 262F 0008 - D3 = return address
        //   MOVEA.L SP, A2         ; 244F - A2 = SP after saves (points to saved regs)
        //   LEA     12(SP), SP     ; 4FEF 000C - SP -> params (skip saves + ret)
        //   trap_word              ; AXXX - Trap pops params
        //   MOVEA.L A2, SP         ; 2E4A - Restore SP to saved regs
        //   MOVEM.L (SP)+, D3/A2   ; 4CDF 1008 - Restore D3, A2 (SP += 8)
        //   JMP     (D3)           ; Wait, can't JMP to Dn!
        //
        // Can't jump to data register. Need to use address register.
        // Use A3 instead of D3:
        //
        //   MOVEM.L A2/A3, -(SP)   ; 48E7 0018 - Save A2, A3 (8 bytes)
        //   MOVEA.L 8(SP), A3      ; 266F 0008 - A3 = return address
        //   MOVEA.L SP, A2         ; 244F - A2 = SP after saves
        //   LEA     12(SP), SP     ; 4FEF 000C - SP -> params
        //   trap_word              ; AXXX - Trap pops params
        //   MOVEA.L A2, SP         ; 2E4A - Restore SP
        //   MOVEM.L (SP)+, A2/A3   ; 4CDF 0018 - Restore A2, A3
        //   JMP     (A3)           ; 4ED3 - Return to caller
        //
        // Total: 24 bytes. That's a lot but it should work.
        //
        // Let's encode this:
        //   MOVEM.L A2/A3, -(SP)   ; 48E7 0018
        //   MOVEA.L 8(SP), A3      ; 266F 0008
        //   MOVEA.L SP, A2         ; 244F
        //   LEA     12(SP), SP     ; 4FEF 000C
        //   trap_word              ; AXXX
        //   MOVEA.L A2, SP         ; 2E4A
        //   MOVEM.L (SP)+, A2/A3   ; 4CDF 0018
        //   JMP     (A3)           ; 4ED3
        //
        // WORKING APPROACH using LINK/UNLK frame:
        //
        // The key insight: LINK creates a stable frame pointer (A6) that
        // survives the trap's stack manipulation. We can store our return
        // address relative to A6.
        //
        // Stack on entry: SP -> [ret_addr][param1][param2]...
        //
        //   LINK    A6, #0         ; 4E56 0000 - Push old A6, A6 = SP
        //                          ; Stack: [old_A6][ret_addr][param1]...
        //                          ; A6 points to old_A6
        //   MOVE.L  4(A6), -(SP)   ; 2F2E 0004 - Push ret_addr below frame
        //                          ; Stack: [ret_addr][old_A6][ret_addr][param1]...
        //   LEA     8(A6), SP      ; 4DEE 0008 - SP -> params (skip frame + orig ret)
        //                          ; Stack unchanged, SP points at param1
        //   trap_word              ; AXXX - Trap pops params
        //   MOVEA.L A6, SP         ; 2E4E - SP -> old_A6 (our frame)
        //   MOVE.L  -(SP), 4(A6)   ; 2D5F 0004 - Pop saved ret to frame's ret slot
        //                          ; (Actually we need a different approach here)
        //
        // Hmm, that's still complex. Let's try an even simpler approach:
        //
        // SIMPLEST: Use A6 frame, save ret addr to local variable.
        //
        //   LINK    A6, #-4        ; 4E56 FFFC - Frame with 4 bytes local
        //                          ; Stack: [local][old_A6][ret_addr][params]
        //                          ; A6 -> old_A6, -4(A6) -> local
        //   MOVE.L  4(A6), -4(A6)  ; 2D6E 0004 FFFC - Save ret to local
        //   LEA     8(A6), SP      ; 4DEE 0008 - SP -> params
        //   trap_word              ; AXXX - Trap pops params, SP moves up
        //   MOVEA.L A6, SP         ; 2E4E - Restore SP to frame
        //   SUBQ.L  #4, SP         ; 5988 - Point to local
        //   MOVE.L  (SP)+, 4(A6)   ; Wait, we've lost track...
        //
        // OK, the complexity is that after the trap, we need to get back
        // to a known good state. Let's use the absolute simplest approach:
        //
        // Save ret addr to A4 (callee-saved), execute trap, restore.
        // Since we use A4, we must save/restore it too.
        // Save both in the frame.
        //
        //   LINK    A6, #-8        ; 4E56 FFF8 - 8 bytes local
        //   MOVE.L  A4, -8(A6)     ; 2D4C FFF8 - Save A4
        //   MOVEA.L 4(A6), A4      ; 286E 0004 - A4 = return address
        //   LEA     8(A6), SP      ; 4DEE 0008 - SP = params
        //   trap_word              ; AXXX - Trap pops params
        //   MOVEA.L A6, SP         ; 2E4E - Restore frame
        //   MOVE.L  -8(A6), A4     ; 286E FFF8 - Restore A4
        //   UNLK    A6             ; 4E5E - Pop frame (SP -> ret_addr)
        //   RTS                    ; 4E75 - Return
        //
        // Wait, after UNLK, SP points where? UNLK does: SP = A6, A6 = (SP)+
        // So SP ends up pointing to what was at 4(old_A6), which is ret_addr.
        // Perfect!
        //
        // But wait - we moved ret_addr to A4, but we never put it back!
        // After UNLK, SP points to the old ret_addr location, but the
        // trap may have overwritten that memory when it popped params.
        //
        // We need to PUT the return address back before UNLK:
        //
        // SIMPLER APPROACH: Use A4 for return address, A3 for saved A4.
        // Don't use LINK - just save registers and jump directly.
        //
        // Stack on entry: SP -> [ret_addr][param1][param2]...
        //
        //   MOVEM.L A3/A4, -(SP)   ; Save A3, A4 (8 bytes pushed)
        //                          ; SP -> [A4][A3][ret_addr][param1]...
        //   MOVEA.L 8(SP), A4      ; A4 = return address
        //   MOVEA.L SP, A3         ; A3 = SP (points to saved regs)
        //   LEA     12(SP), SP     ; SP -> params (skip saved regs + ret)
        //   trap_word              ; Trap pops params, SP moves up
        //   MOVEA.L A3, SP         ; Restore SP to saved regs
        //   MOVEM.L (SP)+, A3/A4   ; Restore A3, A4 (SP += 8)
        //   JMP     (A4)           ; Return to caller (A4 still has ret addr after restore? NO!)
        //
        // Wait, that's wrong - after MOVEM restore, A4 has original A4, not ret addr.
        // We need to save ret addr somewhere that survives the MOVEM restore.
        //
        // NEW APPROACH: Use A3 for original A3, A4 for original A4, and
        // save ret addr in our stack frame which we access via A3.
        //
        //   MOVEM.L A3/A4, -(SP)   ; Save A3, A4 (8 bytes)
        //                          ; Stack: [saved_A4][saved_A3][ret][params]
        //                          ; Offsets: 0(SP)=A4, 4(SP)=A3, 8(SP)=ret, 12(SP)=params
        //   MOVEA.L 8(SP), A4      ; A4 = return address
        //   MOVEA.L SP, A3         ; A3 = SP (our save area)
        //   LEA     12(SP), SP     ; SP -> params
        //   trap_word              ; Trap pops params
        //   MOVEA.L A3, SP         ; Restore SP to save area
        //   MOVEA.L A4, A0         ; Copy ret addr to A0 (scratch, but we use it before trap could clobber more)
        //   MOVEM.L (SP)+, A3/A4   ; Restore A3, A4
        //   JMP     (A0)           ; Jump to return address
        //
        // Hmm, but we need ret addr AFTER restoring A3/A4. Let's save to 8(A3):
        //
        //   MOVEM.L A3/A4, -(SP)   ; Save A3, A4
        //   MOVEA.L 8(SP), A4      ; A4 = return address
        //   MOVEA.L SP, A3         ; A3 = save area
        //   MOVE.L  A4, 8(A3)      ; Store ret addr in its slot (redundant but ensures it's there)
        //   LEA     12(SP), SP     ; SP -> params
        //   trap_word              ; Trap pops params
        //   MOVEA.L A3, SP         ; Restore SP
        //   MOVEA.L 8(SP), A4      ; Re-read ret addr from save area into A4
        //   MOVEM.L (SP)+, A3/A4   ; Restore A3, A4 - wait, this overwrites A4!
        //
        // This is circular. We need a THIRD register or different approach.
        //
        // CLEANEST: Use A2, A3, A4 where:
        //   A2 = saved SP (to restore after trap)
        //   A3 = saved original A2
        //   A4 = return address (keep it here, restore original A4 from stack)
        //
        //   MOVEM.L A2-A4, -(SP)   ; Save A2, A3, A4 (12 bytes)
        //                          ; Stack: [A4][A3][A2][ret][params]
        //   MOVEA.L 12(SP), A4     ; A4 = return address
        //   MOVEA.L SP, A2         ; A2 = save area
        //   LEA     16(SP), SP     ; SP -> params (skip saves + ret)
        //   trap_word              ; Trap pops params
        //   MOVEA.L A2, SP         ; Restore SP to save area
        //   MOVEM.L (SP)+, A2/A3   ; Restore only A2, A3 (SP += 8)
        //   ADDQ.L  #4, SP         ; Skip saved A4 slot
        //   JMP     (A4)           ; Return (A4 has ret addr, caller's A4 was in saved slot we skipped)
        //
        // Wait, we need to restore A4 too! The caller expects it preserved.
        // We can't have our cake and eat it too with A4.
        //
        // FINAL SOLUTION: Push ret addr separately AFTER restoring all regs.
        //
        //   MOVEM.L A2/A3, -(SP)   ; Save A2, A3 (8 bytes)
        //   MOVEA.L 8(SP), A3      ; A3 = return address
        //   MOVEA.L SP, A2         ; A2 = save area
        //   LEA     12(SP), SP     ; SP -> params
        //   trap_word              ; Trap pops params
        //   MOVEA.L A2, SP         ; Restore SP to save area
        //   MOVEM.L (SP)+, A2/A3   ; Restore A2, A3 (A3 now has orig A3, not ret!)
        //
        // Argh, same problem.
        //
        // OK, TRULY FINAL: Use A2 for SP save, put ret addr in stack slot.
        //
        //   MOVE.L  A2, -(SP)      ; Save A2 (4 bytes)
        //                          ; Stack: [saved_A2][ret][params]
        //   MOVEA.L SP, A2         ; A2 = points to saved_A2
        //   ADDQ.L  #8, SP         ; SP -> params (skip saved_A2 + ret)
        //   trap_word              ; Trap pops params
        //   MOVEA.L A2, SP         ; Restore SP to saved_A2
        //   MOVEA.L (SP)+, A2      ; Restore A2
        //   RTS                    ; Return (ret addr is now at top of stack)
        //
        // This is 16 bytes and simple! The key insight:
        // - We save A2 and remember where with A2 itself
        // - After trap, we restore SP to that point
        // - We pop A2 to restore it
        // - The return address is now at (SP), so RTS works
        //
        code.push_back(0x2F);  // MOVE.L A2, -(SP)
        code.push_back(0x0A);
        code.push_back(0x24);  // MOVEA.L SP, A2
        code.push_back(0x4F);
        code.push_back(0x50);  // ADDQ.L #8, SP
        code.push_back(0x8F);
        code.push_back((stub.trapWord >> 8) & 0xFF);  // trap_word
        code.push_back(stub.trapWord & 0xFF);
        code.push_back(0x2E);  // MOVEA.L A2, SP
        code.push_back(0x4A);
        code.push_back(0x24);  // MOVEA.L (SP)+, A2
        code.push_back(0x5F);
        code.push_back(0x4E);  // RTS
        code.push_back(0x75);
        // Pad to 16 bytes
        code.push_back(0x4E);  // NOP
        code.push_back(0x71);
        break;
    }
  }

  if (config && config->verbose && !stubs.empty()) {
    outs() << "Phase 3 complete: " << code.size() << " bytes of stub code generated\n";
  }

  return code;
}

size_t TrapStubs::size() const {
  size_t total = 0;
  for (const auto &stub : stubs) {
    switch (stub.conv) {
      case TRAP_REG_A0:
        total += 8;
        break;
      case TRAP_STACK:
      case TRAP_DISPATCHER:
        total += 16;  // Simple A2-based approach: 16 bytes
        break;
    }
  }
  return total;
}

void TrapStubs::buildOffsetCache() const {
  if (cacheValid)
    return;

  offsetCache.clear();
  uint32_t offset = 0;
  for (const auto &stub : stubs) {
    offsetCache[stub.name] = offset;
    switch (stub.conv) {
      case TRAP_REG_A0:
        offset += 8;  // MOVE.L d(SP),A0 + trap + RTS
        break;
      case TRAP_STACK:
      case TRAP_DISPATCHER:
        offset += 16;  // Simple A2-based approach: 16 bytes
        break;
    }
  }
  cacheValid = true;
}

uint32_t TrapStubs::getStubOffset(llvm::StringRef name) const {
  buildOffsetCache();

  auto it = offsetCache.find(name.str());
  if (it != offsetCache.end())
    return it->second;

  // Should not happen - caller should only ask for stubs that exist
  llvm::errs() << "warning: stub not found: " << name << "\n";
  return 0;
}

} // namespace lld::classic68k
