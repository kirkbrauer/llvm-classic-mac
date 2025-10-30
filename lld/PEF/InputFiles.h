//===- InputFiles.h ---------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_PEF_INPUT_FILES_H
#define LLD_PEF_INPUT_FILES_H

#include "lld/Common/LLVM.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Object/PEFObjectFile.h"
#include "llvm/Support/MemoryBuffer.h"
#include <vector>

namespace lld::pef {

class Symbol;
class InputSection;

// Base class for all input files
class InputFile {
public:
  enum Kind {
    ObjectKind,
    SharedLibraryKind,  // Phase 2: PEF shared library
  };

  virtual ~InputFile() = default;

  // Returns the filename
  StringRef getName() const { return mb.getBufferIdentifier(); }

  Kind kind() const { return fileKind; }

  // Archive name if this file came from an archive
  std::string archiveName;

  // Get all symbols defined or referenced by this file
  ArrayRef<Symbol *> getSymbols() const { return symbols; }
  MutableArrayRef<Symbol *> getMutableSymbols() { return symbols; }

protected:
  InputFile(Kind k, MemoryBufferRef m) : mb(m), fileKind(k) {}

  MemoryBufferRef mb;
  std::vector<Symbol *> symbols;

private:
  const Kind fileKind;
};

// PEF object file (.o)
class ObjFile : public InputFile {
public:
  ObjFile(MemoryBufferRef m, StringRef archiveName = "");

  static bool classof(const InputFile *f) { return f->kind() == ObjectKind; }

  // Parse the PEF object file and extract sections and symbols
  void parse();

  // Get the underlying PEF object file
  llvm::object::PEFObjectFile *getPEFObj() const { return pefObj.get(); }

  // Get section by index
  Expected<llvm::PEF::SectionHeader> getSectionHeader(unsigned index) const {
    return pefObj->getSectionHeader(index);
  }

  // Get number of sections
  unsigned getSectionCount() const { return pefObj->getSectionCount(); }

  // Get section data
  Expected<ArrayRef<uint8_t>> getSectionData(unsigned index) const {
    return pefObj->getSectionData(index);
  }

  // Get input sections
  ArrayRef<InputSection *> getInputSections() const { return inputSections; }

private:
  std::unique_ptr<llvm::object::PEFObjectFile> pefObj;
  std::vector<InputSection *> inputSections;
};

// PEF shared library file (.pef) - Phase 2
class SharedLibraryFile : public InputFile {
public:
  SharedLibraryFile(MemoryBufferRef m, bool isWeak = false);

  static bool classof(const InputFile *f) {
    return f->kind() == SharedLibraryKind;
  }

  // Parse the PEF shared library and extract exported symbols
  void parse();

  // Get the library name (from loader section or filename)
  StringRef getLibraryName() const { return libraryName; }

  // Check if this is a weak import library
  bool isWeakImport() const { return weak; }

  // Get the underlying PEF object file
  llvm::object::PEFObjectFile *getPEFObj() const { return pefLib.get(); }

  // Find an exported symbol by name
  // Returns non-null if found, stores symbol class in lastSymbolClass
  Symbol *findExport(StringRef name) const;

  // Get the symbol class of the last symbol found by findExport()
  uint8_t getLastSymbolClass() const { return lastSymbolClass; }

private:
  std::unique_ptr<llvm::object::PEFObjectFile> pefLib;
  std::string libraryName;
  bool weak;
  mutable uint8_t lastSymbolClass = 0; // Symbol class from last findExport() call
};

// Opens a file and returns its memory buffer
std::optional<MemoryBufferRef> readFile(StringRef path);

// Create an input file from a memory buffer
// Will report error if the buffer is not a valid PEF object file
InputFile *createObjectFile(MemoryBufferRef mb, StringRef archiveName = "");

// Create a shared library file from a memory buffer (Phase 2)
SharedLibraryFile *createSharedLibraryFile(MemoryBufferRef mb,
                                            bool isWeak = false);

} // namespace lld::pef

#endif
