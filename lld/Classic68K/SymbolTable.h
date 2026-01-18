//===- SymbolTable.h - Classic 68K symbol resolution ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file provides symbol table management for the Classic 68K linker.
// It tracks defined and undefined symbols, resolves undefined symbols against
// the Mac Toolbox trap database, and reports unresolved symbols.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_CLASSIC68K_SYMBOLTABLE_H
#define LLD_CLASSIC68K_SYMBOLTABLE_H

#include "TrapDatabase.h"
#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace lld::classic68k {

/// A resolved symbol with its final address
struct ResolvedSymbol {
  std::string name;        ///< Symbol name
  uint32_t address;        ///< Final offset (in code or data area)
  bool isTrap;             ///< True if resolved to A-Trap stub
  bool isData;             ///< True if symbol is in data/BSS section
  uint16_t trapWord;       ///< If isTrap, the A-line trap word
  TrapCallingConv trapConv; ///< If isTrap, the calling convention
  TrapReturnType trapRetType; ///< If isTrap, the return value type
  uint8_t trapParamSize;   ///< If isTrap, total parameter size in bytes (Pascal convention)

  ResolvedSymbol() : address(0), isTrap(false), isData(false), trapWord(0),
                     trapConv(TRAP_STACK), trapRetType(TRAP_RET_VOID),
                     trapParamSize(0) {}
};

/// Symbol table for Classic 68K linking
///
/// Manages defined symbols (from input files), undefined symbols (references),
/// and resolves undefined symbols against the Mac Toolbox trap database.
class SymbolTable {
public:
  /// Add a defined code symbol with its offset in user code
  ///
  /// @param name Symbol name
  /// @param offset Offset within the user code section
  void addDefined(llvm::StringRef name, uint32_t offset);

  /// Add a defined data symbol with its offset in the data area
  ///
  /// @param name Symbol name
  /// @param offset Offset within the data/BSS area
  void addDataSymbol(llvm::StringRef name, uint32_t offset);

  /// Add an undefined symbol reference
  ///
  /// @param name Symbol name that needs to be resolved
  void addUndefined(llvm::StringRef name);

  /// Resolve undefined symbols against the trap database
  ///
  /// For each undefined symbol, attempts to find a matching Mac Toolbox trap.
  /// Resolved traps are converted to ResolvedSymbol entries.
  ///
  /// @param db Trap database to search
  /// @return true if all undefined symbols were resolved
  bool resolveTraps(const TrapDatabase &db);

  /// Get list of unresolved symbol names
  std::vector<std::string> getUnresolvedSymbols() const;

  /// Check if there are any unresolved symbols
  bool hasUnresolvedSymbols() const { return !unresolved.empty(); }

  /// Look up a resolved symbol by name
  ///
  /// @param name Symbol name to look up
  /// @return Pointer to ResolvedSymbol if found, nullptr otherwise
  const ResolvedSymbol *find(llvm::StringRef name) const;

  /// Get all symbols that resolved to traps (for stub generation)
  std::vector<const ResolvedSymbol *> getTrapStubs() const;

  /// Get all defined symbols
  const std::map<std::string, ResolvedSymbol> &getSymbols() const {
    return symbols;
  }

  /// Update addresses after code layout is finalized
  ///
  /// Called after trap stubs are generated to update defined symbol
  /// addresses to account for startup code and stub offsets.
  ///
  /// @param userCodeOffset Offset where user code starts in CODE 1
  void finalizeAddresses(uint32_t userCodeOffset);

private:
  /// All resolved symbols (defined + resolved traps)
  std::map<std::string, ResolvedSymbol> symbols;

  /// Undefined symbols that still need resolution
  std::set<std::string> undefined;

  /// Symbols that could not be resolved
  std::set<std::string> unresolved;
};

} // namespace lld::classic68k

#endif // LLD_CLASSIC68K_SYMBOLTABLE_H
