//===- ELFInputFiles.h - ELF object file input for PEF linker ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares classes for reading ELF object files and converting them
// to PEF format during linking. This allows the compiler to emit standard ELF
// object files while the linker produces PEF executables for Classic Mac OS.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_PEF_ELF_INPUT_FILES_H
#define LLD_PEF_ELF_INPUT_FILES_H

#include "InputFiles.h"
#include "InputSection.h"
#include "lld/Common/LLVM.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/MemoryBuffer.h"
#include <map>
#include <set>
#include <vector>

namespace lld::pef {

class Symbol;

/// Represents a relocation from an ELF object file.
/// These are converted to PEF relocations during linking.
struct ELFRelocation {
  uint64_t offset;      // Offset within the section
  uint32_t type;        // ELF relocation type (R_PPC_*)
  uint32_t symbolIndex; // Index into ELF symbol table
  int64_t addend;       // Addend for RELA relocations

  // The resolved symbol (set during symbol resolution)
  Symbol *symbol = nullptr;
};

/// Input section created from an ELF section.
/// This adapts ELF sections to work with the PEF linker's section model.
class ELFInputSection {
public:
  ELFInputSection(class ELFObjFile *file, unsigned index,
                  llvm::object::SectionRef secRef);

  // Get the owning file
  ELFObjFile *getFile() const { return file; }

  // Get the section index in the input file
  unsigned getIndex() const { return sectionIndex; }

  // Get section name
  StringRef getName() const { return name; }

  // Get section size
  uint64_t getSize() const { return size; }

  // Get section data
  ArrayRef<uint8_t> getData() const { return data; }

  // Get section kind for PEF (code=0, data=1, etc.)
  uint8_t getPEFKind() const { return pefKind; }

  // Get section alignment (power of 2)
  uint32_t getAlignment() const { return alignment; }

  // Virtual address assigned during layout
  uint64_t getVirtualAddress() const { return virtualAddress; }
  void setVirtualAddress(uint64_t addr) { virtualAddress = addr; }

  // Relocations for this section
  ArrayRef<ELFRelocation> getRelocations() const { return relocations; }
  void addRelocation(const ELFRelocation &reloc) {
    relocations.push_back(reloc);
  }

  // Patched data support (after relocation processing)
  bool hasPatchedData() const { return !patchedData.empty(); }
  ArrayRef<uint8_t> getPatchedData() const { return patchedData; }
  std::vector<uint8_t> &getMutablePatchedData() { return patchedData; }
  void initializePatchedData() {
    if (patchedData.empty() && !data.empty()) {
      patchedData.assign(data.begin(), data.end());
    }
  }

  // Check if this is a code section
  bool isCode() const { return pefKind == 0; }

  // Check if this is a data section
  bool isData() const { return pefKind == 1 || pefKind == 2; }

private:
  ELFObjFile *file;
  unsigned sectionIndex;
  StringRef name;
  uint64_t size;
  ArrayRef<uint8_t> data;
  std::vector<uint8_t> bssData;  // Storage for BSS section zero-fill
  uint8_t pefKind;           // PEF section kind (code=0, data=1, pidata=2, constant=3)
  uint32_t alignment;
  uint64_t virtualAddress = 0;
  std::vector<ELFRelocation> relocations;
  std::vector<uint8_t> patchedData;
};

/// ELF object file (.o) reader for PEF linker.
/// Parses ELF object files and extracts sections, symbols, and relocations
/// in a form suitable for PEF generation.
class ELFObjFile : public InputFile {
public:
  ELFObjFile(MemoryBufferRef m, StringRef archiveName = "");

  static bool classof(const InputFile *f) { return f->kind() == ELFObjectKind; }

  // Parse the ELF object file
  void parse();

  // Get input sections as ELFInputSection (for relocation processing)
  ArrayRef<ELFInputSection *> getELFInputSections() const {
    return elfInputSections;
  }

  // Get input sections as InputSection (for Writer.cpp compatibility)
  ArrayRef<InputSection *> getInputSections() const {
    return inputSections;
  }

  // Get the underlying ELF object file
  llvm::object::ObjectFile *getELFObj() const { return elfObj.get(); }

  // Get symbol for ELF symbol index
  Symbol *getSymbolByIndex(uint32_t index) const {
    auto it = elfSymbolMap.find(index);
    return it != elfSymbolMap.end() ? it->second : nullptr;
  }

  // Get set of functions whose addresses are taken (need TVectors)
  const std::set<Symbol *> &getAddressTakenFunctions() const {
    return addressTakenFunctions;
  }

  // Mark a function as having its address taken
  void markAddressTaken(Symbol *sym) { addressTakenFunctions.insert(sym); }

private:
  void parseSections();
  void parseSymbols();
  void parseRelocations();

  // Map ELF section type/flags to PEF section kind
  uint8_t mapELFSectionToPEFKind(uint32_t type, uint64_t flags) const;

  std::unique_ptr<llvm::object::ObjectFile> elfObj;
  std::vector<ELFInputSection *> elfInputSections;
  std::vector<InputSection *> inputSections;  // InputSection wrappers for Writer.cpp

  // Map from ELF symbol index to Symbol
  std::map<uint32_t, Symbol *> elfSymbolMap;

  // Map from ELF section index to ELFInputSection
  std::map<uint32_t, ELFInputSection *> sectionMap;

  // Map from ELF section index to InputSection (for relocation copying)
  std::map<uint32_t, InputSection *> inputSectionMap;

  // Functions whose addresses are taken (used as function pointers)
  std::set<Symbol *> addressTakenFunctions;
};

// Create an ELF object file from a memory buffer
ELFObjFile *createELFObjectFile(MemoryBufferRef mb, StringRef archiveName = "");

} // namespace lld::pef

#endif // LLD_PEF_ELF_INPUT_FILES_H
