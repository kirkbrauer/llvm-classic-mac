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
#include "llvm/Object/XCOFFObjectFile.h"
#include "llvm/Support/MemoryBuffer.h"
#include <map>
#include <set>
#include <vector>

namespace lld::pef {

class Symbol;
class InputSection;

// Base class for all input files
class InputFile {
public:
  enum Kind {
    ObjectKind,         // Legacy PEF object file (deprecated)
    ELFObjectKind,      // ELF object file (primary format)
    XCOFFObjectKind,    // XCOFF object file (for MPW static libraries)
    SharedLibraryKind,  // PEF shared library (for Toolbox imports)
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

  // Get symbol for local import index (used for relocation remapping)
  Symbol *getImportSymbol(uint32_t localIndex) const {
    auto it = importIndexMap.find(localIndex);
    return it != importIndexMap.end() ? it->second : nullptr;
  }

  // Get set of functions whose addresses are taken (need TVectors)
  const std::set<Symbol*>& getAddressTakenFunctions() const {
    return addressTakenFunctions;
  }

private:
  std::unique_ptr<llvm::object::PEFObjectFile> pefObj;
  std::vector<InputSection *> inputSections;

  // Map from local import index (in this object file) to Symbol
  // Used to remap import indices when generating final relocations
  std::map<uint32_t, Symbol*> importIndexMap;

  // Set of functions whose addresses are taken (used as function pointers)
  // These functions need TVectors generated in the final binary
  std::set<Symbol*> addressTakenFunctions;

  friend class InputSection;  // Allow access to importIndexMap during parsing
};

// XCOFF object file (.o) - for MPW static libraries like OpenTransportAppPPC.o
class XCOFFObjFile : public InputFile {
public:
  XCOFFObjFile(MemoryBufferRef m, StringRef archiveName = "");

  static bool classof(const InputFile *f) { return f->kind() == XCOFFObjectKind; }

  // Parse the XCOFF object file and extract sections and symbols
  void parse();

  // Get the underlying XCOFF object file
  llvm::object::XCOFFObjectFile *getXCOFFObj() const { return xcoffObj.get(); }

  // Get input sections
  ArrayRef<InputSection *> getInputSections() const { return inputSections; }

  // Get symbol for local import index (used for relocation remapping)
  Symbol *getImportSymbol(uint32_t localIndex) const {
    auto it = importIndexMap.find(localIndex);
    return it != importIndexMap.end() ? it->second : nullptr;
  }

private:
  void parseSections();
  void parseSymbols();
  void parseRelocations();

  std::unique_ptr<llvm::object::XCOFFObjectFile> xcoffObj;
  std::vector<InputSection *> inputSections;

  // Map from XCOFF section index to InputSection
  // Used to find the correct InputSection when processing relocations
  std::map<unsigned, InputSection*> sectionMap;

  // Map from local symbol index to Symbol
  // Used to resolve relocations that reference symbols by index
  std::map<uint32_t, Symbol*> importIndexMap;
};

// PEF shared library file (.pef) - Phase 2
// Supports concatenated PEF containers (multiple 'Joy!' headers in one file)
class SharedLibraryFile : public InputFile {
public:
  SharedLibraryFile(MemoryBufferRef m, bool isWeak = false);

  static bool classof(const InputFile *f) {
    return f->kind() == SharedLibraryKind;
  }

  // Parse the PEF shared library and extract exported symbols
  // Handles concatenated PEF containers (e.g., OpenTransportLib has 9)
  void parse();

  // Get the library name (from loader section or filename)
  StringRef getLibraryName() const { return libraryName; }

  // Check if this is a weak import library
  bool isWeakImport() const { return weak; }

  // Get the number of PEF containers in this library
  unsigned getContainerCount() const { return pefContainers.size(); }

  // Get a specific PEF container by index
  llvm::object::PEFObjectFile *getContainer(unsigned index) const {
    return index < pefContainers.size() ? pefContainers[index].get() : nullptr;
  }

  // Find an exported symbol by name (searches all containers)
  // Returns non-null if found, stores symbol class in lastSymbolClass
  Symbol *findExport(StringRef name) const;

  // Get the symbol class of the last symbol found by findExport()
  uint8_t getLastSymbolClass() const { return lastSymbolClass; }

private:
  // Find export within a specific container
  Symbol *findExportInContainer(llvm::object::PEFObjectFile *pef,
                                 StringRef name) const;

  // Multiple PEF containers (for concatenated files like OpenTransportLib)
  std::vector<std::unique_ptr<llvm::object::PEFObjectFile>> pefContainers;
  std::string libraryName;
  bool weak;
  mutable uint8_t lastSymbolClass = 0; // Symbol class from last findExport() call
};

// Opens a file and returns its memory buffer
std::optional<MemoryBufferRef> readFile(StringRef path);

// Create an input file from a memory buffer
// Will report error if the buffer is not a valid PEF object file
InputFile *createObjectFile(MemoryBufferRef mb, StringRef archiveName = "");

// Process an archive (.rlib, .a) and return all object files within
// Used for Rust rlib archives and static libraries
std::vector<InputFile *> createObjectFilesFromArchive(MemoryBufferRef mb);

// Create an XCOFF object file from a memory buffer
// Used for MPW static libraries like OpenTransportAppPPC.o
InputFile *createXCOFFObjectFile(MemoryBufferRef mb, StringRef archiveName = "");

// Create a shared library file from a memory buffer (Phase 2)
SharedLibraryFile *createSharedLibraryFile(MemoryBufferRef mb,
                                            bool isWeak = false);

} // namespace lld::pef

#endif
