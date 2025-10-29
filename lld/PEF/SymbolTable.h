//===- SymbolTable.h --------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_PEF_SYMBOL_TABLE_H
#define LLD_PEF_SYMBOL_TABLE_H

#include "Symbols.h"
#include "lld/Common/LLVM.h"
#include "llvm/ADT/CachedHashString.h"
#include "llvm/ADT/DenseMap.h"
#include <vector>

namespace lld::pef {

class InputFile;

// Global symbol table
class SymbolTable {
public:
  // Insert a symbol into the table
  // Returns the symbol from the table (may be existing symbol if already defined)
  Symbol *insert(StringRef name, InputFile *file);

  // Add a defined symbol
  Defined *addDefined(StringRef name, InputFile *file, uint32_t value,
                      int16_t sectionIndex, uint8_t symbolClass);

  // Add an undefined symbol
  Undefined *addUndefined(StringRef name, InputFile *file,
                          uint8_t symbolClass = 0);

  // Look up a symbol
  Symbol *find(StringRef name);

  // Get all defined symbols
  std::vector<Defined *> getDefinedSymbols() const;

  // Get all undefined symbols
  std::vector<Undefined *> getUndefinedSymbols() const;

  // Get all symbols
  const llvm::DenseMap<llvm::CachedHashStringRef, Symbol *> &
  getSymbols() const {
    return symMap;
  }

private:
  llvm::DenseMap<llvm::CachedHashStringRef, Symbol *> symMap;
  std::vector<Symbol *> symVector;
};

// Global symbol table instance
extern SymbolTable *symtab;

} // namespace lld::pef

#endif
