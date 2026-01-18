//===- SymbolTable.cpp - Classic 68K symbol resolution --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "SymbolTable.h"
#include "Config.h"
#include "lld/Common/ErrorHandler.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace lld;

namespace lld::classic68k {

void SymbolTable::addDefined(llvm::StringRef name, uint32_t offset) {
  std::string nameStr = name.str();

  // Remove from undefined if it was there
  undefined.erase(nameStr);

  // Add to symbols table
  ResolvedSymbol sym;
  sym.name = nameStr;
  sym.address = offset;
  sym.isTrap = false;
  sym.isData = false;
  sym.trapWord = 0;
  sym.trapConv = TRAP_STACK;

  symbols[nameStr] = sym;
}

void SymbolTable::addDataSymbol(llvm::StringRef name, uint32_t offset) {
  std::string nameStr = name.str();

  // Remove from undefined if it was there
  undefined.erase(nameStr);

  // Add to symbols table as a data symbol
  ResolvedSymbol sym;
  sym.name = nameStr;
  sym.address = offset;
  sym.isTrap = false;
  sym.isData = true;
  sym.trapWord = 0;
  sym.trapConv = TRAP_STACK;

  symbols[nameStr] = sym;
}

void SymbolTable::addUndefined(llvm::StringRef name) {
  std::string nameStr = name.str();

  // Skip if already defined
  if (symbols.count(nameStr))
    return;

  undefined.insert(nameStr);
}

bool SymbolTable::resolveTraps(const TrapDatabase &db) {
  unresolved.clear();

  if (config && config->verbose) {
    errorHandler().outs() << "\n=== Phase 2: Resolving Traps ===\n";
    errorHandler().outs() << "Undefined symbols to resolve: " << undefined.size() << "\n";
  }

  size_t resolvedCount = 0;
  for (const auto &name : undefined) {
    const TrapInfo *trap = db.findTrap(name);
    if (trap) {
      // Resolved to a trap - will be added to stub table
      ResolvedSymbol sym;
      sym.name = name;
      sym.address = 0;  // Will be set when stubs are generated
      sym.isTrap = true;
      sym.trapWord = trap->trapWord;
      sym.trapConv = trap->conv;
      sym.trapRetType = trap->retType;
      sym.trapParamSize = trap->paramSize;

      symbols[name] = sym;
      resolvedCount++;

      if (config && config->verbose) {
        errorHandler().outs() << "  Resolved: " << name
               << " -> 0x" << format_hex_no_prefix(trap->trapWord, 4)
               << " (" << (trap->conv == TRAP_REG_A0 ? "REG_A0" :
                          trap->conv == TRAP_STACK ? "STACK" : "DISPATCHER")
               << ", ret=" << (trap->retType == TRAP_RET_VOID ? "void" :
                               trap->retType == TRAP_RET_POINTER ? "ptr" :
                               trap->retType == TRAP_RET_WORD ? "word" : "byte")
               << ", params=" << (unsigned)trap->paramSize << ")\n";
      }
    } else {
      // Could not resolve
      unresolved.insert(name);
      if (config && config->verbose) {
        errorHandler().outs() << "  Unresolved: " << name << "\n";
      }
    }
  }

  if (config && config->verbose) {
    errorHandler().outs() << "Phase 2 complete: " << resolvedCount << " traps resolved";
    if (!unresolved.empty()) {
      errorHandler().outs() << ", " << unresolved.size() << " unresolved";
    }
    errorHandler().outs() << "\n";
  }

  undefined.clear();
  return unresolved.empty();
}

std::vector<std::string> SymbolTable::getUnresolvedSymbols() const {
  return std::vector<std::string>(unresolved.begin(), unresolved.end());
}

const ResolvedSymbol *SymbolTable::find(llvm::StringRef name) const {
  auto it = symbols.find(name.str());
  if (it != symbols.end())
    return &it->second;
  return nullptr;
}

std::vector<const ResolvedSymbol *> SymbolTable::getTrapStubs() const {
  std::vector<const ResolvedSymbol *> stubs;
  for (const auto &[name, sym] : symbols) {
    if (sym.isTrap)
      stubs.push_back(&sym);
  }
  return stubs;
}

void SymbolTable::finalizeAddresses(uint32_t userCodeOffset) {
  for (auto &[name, sym] : symbols) {
    if (!sym.isTrap) {
      // User-defined symbols: add offset for startup code and stubs
      sym.address += userCodeOffset;
    }
    // Trap stub addresses are set separately by TrapStubs class
  }
}

} // namespace lld::classic68k
