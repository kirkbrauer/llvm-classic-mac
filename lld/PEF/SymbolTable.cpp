//===- SymbolTable.cpp ----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "SymbolTable.h"
#include "Config.h"
#include "InputFiles.h"
#include "lld/Common/ErrorHandler.h"
#include "lld/Common/Memory.h"
#include "llvm/ADT/StringExtras.h"

using namespace llvm;
using namespace lld;
using namespace lld::pef;

SymbolTable *lld::pef::symtab;

Symbol *SymbolTable::insert(StringRef name, InputFile *file) {
  auto [it, inserted] = symMap.try_emplace(CachedHashStringRef(name), nullptr);
  if (inserted) {
    // New symbol - will be filled in by caller
    return nullptr;
  }
  return it->second;
}

Defined *SymbolTable::addDefined(StringRef name, InputFile *file,
                                 uint32_t value, int16_t sectionIndex,
                                 uint8_t symbolClass) {
  Symbol *existing = insert(name, file);

  if (existing) {
    // Symbol already exists
    if (existing->isDefined()) {
      // Multiple definitions
      if (!config->allowUndefined) {
        error("duplicate symbol: " + name + "\n>>> defined in " +
              existing->getFile()->getName().str() + "\n>>> defined in " +
              file->getName().str());
      }
      return cast<Defined>(existing);
    } else {
      // Was undefined, now defined - replace it
      auto *def = make<Defined>(name, file, value, sectionIndex, symbolClass);
      symMap[CachedHashStringRef(name)] = def;

      if (config->verbose) {
        errorHandler().outs() << "  Resolved undefined symbol: " << name
                             << "\n";
      }

      return def;
    }
  }

  // New symbol
  auto *sym = make<Defined>(name, file, value, sectionIndex, symbolClass);
  symMap[CachedHashStringRef(name)] = sym;
  symVector.push_back(sym);

  if (config->verbose) {
    errorHandler().outs() << "  Defined symbol: " << name
                         << " (section=" << sectionIndex
                         << ", value=0x" << llvm::utohexstr(value) << ")\n";
  }

  return sym;
}

Undefined *SymbolTable::addUndefined(StringRef name, InputFile *file,
                                     uint8_t symbolClass) {
  Symbol *existing = insert(name, file);

  if (existing) {
    // Symbol already exists
    if (existing->isDefined()) {
      // Already defined, return it as-is (no undefined symbol needed)
      return nullptr;
    } else {
      // Already undefined, return existing
      return cast<Undefined>(existing);
    }
  }

  // New undefined symbol
  auto *sym = make<Undefined>(name, file, symbolClass);
  symMap[CachedHashStringRef(name)] = sym;
  symVector.push_back(sym);

  if (config->verbose) {
    errorHandler().outs() << "  Undefined symbol: " << name << "\n";
  }

  return sym;
}

Symbol *SymbolTable::find(StringRef name) {
  auto it = symMap.find(CachedHashStringRef(name));
  if (it == symMap.end())
    return nullptr;
  return it->second;
}

std::vector<Defined *> SymbolTable::getDefinedSymbols() const {
  std::vector<Defined *> result;
  for (Symbol *sym : symVector) {
    if (auto *def = dyn_cast<Defined>(sym))
      result.push_back(def);
  }
  return result;
}

std::vector<Undefined *> SymbolTable::getUndefinedSymbols() const {
  std::vector<Undefined *> result;
  for (Symbol *sym : symVector) {
    if (auto *undef = dyn_cast<Undefined>(sym))
      result.push_back(undef);
  }
  return result;
}
