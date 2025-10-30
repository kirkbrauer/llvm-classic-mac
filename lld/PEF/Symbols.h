//===- Symbols.h ------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_PEF_SYMBOLS_H
#define LLD_PEF_SYMBOLS_H

#include "lld/Common/LLVM.h"
#include "llvm/BinaryFormat/PEF.h"
#include <cstdint>

namespace lld::pef {

class InputFile;
class InputSection;
class SharedLibraryFile;  // Forward declaration for Phase 2

#define INVALID_INDEX UINT32_MAX

// Base class for all symbol types
class Symbol {
public:
  enum Kind : uint8_t {
    DefinedKind,
    UndefinedKind,
    ImportedKind,  // Phase 2: Imported from shared library
  };

  Symbol(StringRef name, Kind k, InputFile *f)
      : name(name), file(f), symbolKind(k) {}

  virtual ~Symbol() = default;

  Kind kind() const { return symbolKind; }
  bool isDefined() const { return symbolKind == DefinedKind; }
  bool isUndefined() const { return symbolKind == UndefinedKind; }
  bool isImported() const { return symbolKind == ImportedKind; }

  StringRef getName() const { return name; }
  InputFile *getFile() const { return file; }

protected:
  StringRef name;
  InputFile *file;
  Kind symbolKind;
};

// Defined symbol (exported from an object file)
class Defined : public Symbol {
public:
  Defined(StringRef name, InputFile *f, uint32_t value, int16_t sectionIndex,
          uint8_t symbolClass)
      : Symbol(name, DefinedKind, f), value(value), sectionIndex(sectionIndex),
        symbolClass(symbolClass) {}

  static bool classof(const Symbol *s) { return s->kind() == DefinedKind; }

  // Symbol value (offset within section)
  uint32_t getValue() const { return value; }
  void setValue(uint32_t v) { value = v; }

  // Section index (-1 = absolute, -2 = undefined)
  int16_t getSectionIndex() const { return sectionIndex; }
  void setSectionIndex(int16_t idx) { sectionIndex = idx; }

  // PEF symbol class (code, data, tvector, toc, glue)
  uint8_t getSymbolClass() const { return symbolClass; }

  // Output symbol address (set during layout)
  uint64_t getVirtualAddress() const { return virtualAddress; }
  void setVirtualAddress(uint64_t addr) { virtualAddress = addr; }

private:
  uint32_t value;
  int16_t sectionIndex;
  uint8_t symbolClass;
  uint64_t virtualAddress = 0;
};

// Undefined symbol (imported from library)
class Undefined : public Symbol {
public:
  Undefined(StringRef name, InputFile *f, uint8_t symbolClass = 0)
      : Symbol(name, UndefinedKind, f), symbolClass(symbolClass) {}

  static bool classof(const Symbol *s) { return s->kind() == UndefinedKind; }

  uint8_t getSymbolClass() const { return symbolClass; }

private:
  uint8_t symbolClass;
};

// Imported symbol from shared library (Phase 2)
class ImportedSymbol : public Symbol {
public:
  ImportedSymbol(StringRef name, SharedLibraryFile *lib, uint8_t symbolClass,
                 bool weak = false)
      : Symbol(name, ImportedKind, reinterpret_cast<InputFile *>(lib)),
        symbolClass(symbolClass), weak(weak), importIndex(INVALID_INDEX) {}

  static bool classof(const Symbol *s) { return s->kind() == ImportedKind; }

  // Get the source library
  SharedLibraryFile *getLibrary() const {
    return reinterpret_cast<SharedLibraryFile *>(file);
  }

  // PEF symbol class (code, data, tvector, toc, glue)
  uint8_t getSymbolClass() const { return symbolClass; }

  // Check if this is a weak import
  bool isWeakImport() const { return weak; }

  // Import index in the combined import table (set during Writer phase)
  uint32_t getImportIndex() const { return importIndex; }
  void setImportIndex(uint32_t idx) { importIndex = idx; }

  // Virtual address where import will be patched (set during relocation)
  uint64_t getVirtualAddress() const { return virtualAddress; }
  void setVirtualAddress(uint64_t addr) { virtualAddress = addr; }

private:
  uint8_t symbolClass;
  bool weak;
  uint32_t importIndex;
  uint64_t virtualAddress = 0;
};

} // namespace lld::pef

#endif
