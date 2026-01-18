//===- TrapStubs.h - Mac Toolbox A-Trap stub generation -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file provides generation of A-Trap stub code for the Classic 68K linker.
// Stubs are callable functions that bridge C calling conventions to Mac traps:
//   - Stack-based traps: 4 bytes (trap word + RTS)
//   - Register A0 traps: 8 bytes (MOVE.L param to A0 + trap + RTS)
//
//===----------------------------------------------------------------------===//

#ifndef LLD_CLASSIC68K_TRAPSTUBS_H
#define LLD_CLASSIC68K_TRAPSTUBS_H

#include "TrapDatabase.h"
#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace lld::classic68k {

/// Generator for Mac Toolbox A-Trap stubs
///
/// Stubs are callable functions placed in CODE 1 between the startup
/// prologue and user code. They bridge C calling conventions to Mac traps:
///
/// Stack-based traps (4 bytes):
///   - trap_word + RTS
///
/// Register A0 traps (8 bytes):
///   - MOVE.L 4(SP), A0 + trap_word + RTS
///
/// The compiler generates JSR/BSR instructions to these stubs.
class TrapStubs {
public:
  /// Add a trap stub to generate
  ///
  /// @param name Symbol name for this stub
  /// @param trapWord A-line trap word (0xA000-0xAFFF)
  /// @param conv Calling convention (for future use)
  void addStub(llvm::StringRef name, uint16_t trapWord, TrapCallingConv conv);

  /// Generate all stub code
  ///
  /// Stub sizes vary by calling convention (4 or 8 bytes).
  /// Stubs are emitted in the order they were added.
  ///
  /// @return Vector of stub machine code
  std::vector<uint8_t> generate() const;

  /// Get offset of a specific stub within generated block
  ///
  /// @param name Symbol name
  /// @return Offset in bytes from start of stub block
  uint32_t getStubOffset(llvm::StringRef name) const;

  /// Total size of all stubs in bytes
  size_t size() const;

  /// Number of stubs
  size_t count() const { return stubs.size(); }

private:
  struct Stub {
    std::string name;
    uint16_t trapWord;
    TrapCallingConv conv;
  };

  std::vector<Stub> stubs;
  mutable std::map<std::string, uint32_t> offsetCache;
  mutable bool cacheValid = false;

  void buildOffsetCache() const;
};

} // namespace lld::classic68k

#endif // LLD_CLASSIC68K_TRAPSTUBS_H
