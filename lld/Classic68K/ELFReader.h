//===- ELFReader.h - M68k ELF object file reader -----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file reads M68k ELF object files and extracts code, data, and symbol
// information for linking into classic 68K applications.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_CLASSIC68K_ELFREADER_H
#define LLD_CLASSIC68K_ELFREADER_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Object/ELF.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Support/MemoryBuffer.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lld::classic68k {

// Symbol information from ELF
struct ELFSymbol {
  std::string name;
  uint64_t value;
  uint64_t size;
  uint8_t type;        // STT_FUNC, STT_OBJECT, etc.
  uint8_t binding;     // STB_LOCAL, STB_GLOBAL, etc.
  uint16_t sectionIdx;
  bool isFunction() const;
  bool isGlobal() const;
};

// Section information from ELF
struct ELFSection {
  std::string name;
  uint64_t address;
  uint64_t size;
  uint32_t type;       // SHT_PROGBITS, SHT_NOBITS, etc.
  uint64_t flags;      // SHF_ALLOC, SHF_EXECINSTR, etc.
  std::vector<uint8_t> data;

  bool isCode() const;
  bool isData() const;
  bool isRodata() const;  // Read-only data (string literals, etc.)
  bool isBSS() const;
};

// Relocation information
struct ELFRelocation {
  uint64_t offset;     // Offset in section
  uint32_t type;       // R_68K_32, R_68K_PC32, etc.
  uint32_t symbolIdx;  // Symbol index
  int64_t addend;
};

class ELFReader {
public:
  ELFReader() = default;

  // Load an ELF file
  bool load(llvm::StringRef path);

  // Get sections
  const std::vector<ELFSection> &getSections() const { return sections; }

  // Get symbols
  const std::vector<ELFSymbol> &getSymbols() const { return symbols; }

  // Get relocations for a section
  const std::vector<ELFRelocation> &getRelocations(size_t sectionIdx) const;

  // Find a symbol by name
  const ELFSymbol *findSymbol(llvm::StringRef name) const;

  // Get the code section (.text)
  const ELFSection *getTextSection() const;

  // Get the data section (.data)
  const ELFSection *getDataSection() const;

  // Get the read-only data section (.rodata)
  const ELFSection *getRodataSection() const;

  // Get the BSS section (.bss)
  const ELFSection *getBSSSection() const;

  // Get undefined symbols (external references that need resolution)
  std::vector<const ELFSymbol *> getUndefinedSymbols() const;

  // Get symbol by index (for relocation processing)
  const ELFSymbol *getSymbolByIndex(uint32_t idx) const;

  // Get index of .text section
  size_t getTextSectionIndex() const;

  // Get error message
  llvm::StringRef getError() const { return errorMsg; }

private:
  std::unique_ptr<llvm::MemoryBuffer> buffer;
  std::vector<ELFSection> sections;
  std::vector<ELFSymbol> symbols;
  std::map<size_t, std::vector<ELFRelocation>> relocations;
  std::string errorMsg;

  template <typename ELFT>
  bool loadImpl(const llvm::object::ELFFile<ELFT> &elf);
};

} // namespace lld::classic68k

#endif // LLD_CLASSIC68K_ELFREADER_H
