//===- TrapDatabase.h - Mac Toolbox trap lookup ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file provides a lookup table for Mac Toolbox A-Trap definitions.
// The trap table is generated from Traps.h using generate_trap_table.py.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_CLASSIC68K_TRAPDATABASE_H
#define LLD_CLASSIC68K_TRAPDATABASE_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <cstdint>

namespace lld::classic68k {

/// Calling convention for Mac Toolbox traps
enum TrapCallingConv : uint8_t {
  /// Parameters passed on stack (most common)
  TRAP_STACK = 0,

  /// Parameter passed in A0 register (Memory Manager, some File Manager)
  TRAP_REG_A0 = 1,

  /// Selector-based dispatcher (Gestalt, Sound, etc.)
  TRAP_DISPATCHER = 2,
};

/// Return type for Mac Toolbox traps (Pascal calling convention)
enum TrapReturnType : uint8_t {
  /// No return value (void function)
  TRAP_RET_VOID = 0,

  /// Returns pointer/handle (4 bytes) - use MOVEA.L (SP)+, A0
  TRAP_RET_POINTER = 1,

  /// Returns byte/Boolean (1 byte) - use MOVE.B (SP)+, D0
  TRAP_RET_BYTE = 2,

  /// Returns word/short (2 bytes) - use MOVE.W (SP)+, D0
  TRAP_RET_WORD = 3,
};

/// Information about a single Mac Toolbox trap
struct TrapInfo {
  const char *name;        ///< Trap name without underscore (e.g., "SysBeep")
  uint16_t trapWord;       ///< A-line trap word (e.g., 0xA9C8)
  TrapCallingConv conv;    ///< Calling convention
  TrapReturnType retType;  ///< Return value type (for Pascal convention)
  uint8_t paramSize;       ///< Total size of parameters in bytes (for stack restoration)
};

/// Database of Mac Toolbox A-Trap definitions
///
/// This class provides lookup functionality for resolving undefined symbols
/// to their corresponding A-Trap words. The trap table is generated from
/// the Mac Toolbox Traps.h header file.
class TrapDatabase {
public:
  /// Look up a trap by symbol name
  ///
  /// Searches for the trap by exact name match, then tries stripping
  /// a leading underscore (for C symbols like "_SysBeep").
  ///
  /// @param name Symbol name to look up
  /// @return Pointer to TrapInfo if found, nullptr otherwise
  const TrapInfo *findTrap(llvm::StringRef name) const;

  /// Get all traps in the database
  llvm::ArrayRef<TrapInfo> getAllTraps() const;

private:
  static const TrapInfo trapTable[];
  static const size_t trapTableSize;
};

} // namespace lld::classic68k

#endif // LLD_CLASSIC68K_TRAPDATABASE_H
