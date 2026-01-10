//===- ELFReader.cpp - M68k ELF object file reader ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ELFReader.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELF.h"
#include "llvm/Object/ELFTypes.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"

using namespace llvm;
using namespace llvm::object;
using namespace llvm::ELF;

namespace lld::classic68k {

bool ELFSymbol::isFunction() const { return type == STT_FUNC; }

bool ELFSymbol::isGlobal() const {
  return binding == STB_GLOBAL || binding == STB_WEAK;
}

bool ELFSection::isCode() const {
  return (flags & SHF_EXECINSTR) && (flags & SHF_ALLOC);
}

bool ELFSection::isData() const {
  return (flags & SHF_ALLOC) && (flags & SHF_WRITE) && type == SHT_PROGBITS;
}

bool ELFSection::isBSS() const {
  return (flags & SHF_ALLOC) && (flags & SHF_WRITE) && type == SHT_NOBITS;
}

bool ELFReader::load(StringRef path) {
  // Read the file
  auto bufOrErr = MemoryBuffer::getFile(path);
  if (!bufOrErr) {
    errorMsg = "cannot open file: " + path.str();
    return false;
  }
  buffer = std::move(*bufOrErr);

  // Check if it's an ELF file
  StringRef data = buffer->getBuffer();
  if (data.size() < 4 || data[0] != 0x7f || data[1] != 'E' || data[2] != 'L' ||
      data[3] != 'F') {
    errorMsg = "not an ELF file: " + path.str();
    return false;
  }

  // Check class (32-bit or 64-bit)
  uint8_t elfClass = data[4];
  if (elfClass == ELFCLASS32) {
    // Check endianness
    uint8_t dataEncoding = data[5];
    if (dataEncoding == ELFDATA2MSB) {
      // Big-endian 32-bit (M68k)
      auto elfOrErr = ELF32BEFile::create(data);
      if (!elfOrErr) {
        errorMsg = "invalid ELF file";
        return false;
      }
      return loadImpl(*elfOrErr);
    } else if (dataEncoding == ELFDATA2LSB) {
      // Little-endian 32-bit (not M68k, but accept for testing)
      auto elfOrErr = ELF32LEFile::create(data);
      if (!elfOrErr) {
        errorMsg = "invalid ELF file";
        return false;
      }
      return loadImpl(*elfOrErr);
    }
  }

  errorMsg = "unsupported ELF format";
  return false;
}

template <typename ELFT>
bool ELFReader::loadImpl(const ELFFile<ELFT> &elf) {
  // Check machine type
  const auto &header = elf.getHeader();

  if (header.e_machine != EM_68K) {
    errorMsg = "not an M68k ELF file";
    return false;
  }

  // Get section header string table
  auto sectionsOrErr = elf.sections();
  if (!sectionsOrErr) {
    errorMsg = "cannot read sections";
    return false;
  }

  auto shstrtabOrErr = elf.getSectionStringTable(*sectionsOrErr);
  if (!shstrtabOrErr) {
    errorMsg = "cannot read section string table";
    return false;
  }
  StringRef shstrtab = *shstrtabOrErr;

  // Load sections
  size_t idx = 0;
  for (const auto &shdr : *sectionsOrErr) {
    ELFSection sec;

    auto nameOrErr = elf.getSectionName(shdr, shstrtab);
    if (nameOrErr)
      sec.name = nameOrErr->str();

    sec.address = shdr.sh_addr;
    sec.size = shdr.sh_size;
    sec.type = shdr.sh_type;
    sec.flags = shdr.sh_flags;

    // Load section data if it has contents
    if (shdr.sh_type != SHT_NOBITS && shdr.sh_size > 0) {
      auto dataOrErr = elf.getSectionContents(shdr);
      if (dataOrErr) {
        sec.data.assign(dataOrErr->begin(), dataOrErr->end());
      }
    }

    sections.push_back(std::move(sec));
    idx++;
  }

  // Load symbols
  for (const auto &shdr : *sectionsOrErr) {
    if (shdr.sh_type != SHT_SYMTAB && shdr.sh_type != SHT_DYNSYM)
      continue;

    auto symsOrErr = elf.symbols(&shdr);
    if (!symsOrErr)
      continue;

    // Get string table for this symbol table
    auto strtabOrErr = elf.getStringTableForSymtab(shdr, *sectionsOrErr);
    if (!strtabOrErr)
      continue;
    StringRef strtab = *strtabOrErr;

    for (const auto &sym : *symsOrErr) {
      ELFSymbol s;

      auto nameOrErr = sym.getName(strtab);
      if (nameOrErr)
        s.name = nameOrErr->str();

      s.value = sym.st_value;
      s.size = sym.st_size;
      s.type = sym.getType();
      s.binding = sym.getBinding();
      s.sectionIdx = sym.st_shndx;

      symbols.push_back(std::move(s));
    }
  }

  // Load relocations
  idx = 0;
  for (const auto &shdr : *sectionsOrErr) {
    if (shdr.sh_type != SHT_RELA && shdr.sh_type != SHT_REL)
      continue;

    size_t targetSection = shdr.sh_info;

    if (shdr.sh_type == SHT_RELA) {
      auto relasOrErr = elf.relas(shdr);
      if (!relasOrErr)
        continue;

      for (const auto &rela : *relasOrErr) {
        ELFRelocation reloc;
        reloc.offset = rela.r_offset;
        reloc.type = rela.getType(false);
        reloc.symbolIdx = rela.getSymbol(false);
        reloc.addend = rela.r_addend;
        relocations[targetSection].push_back(reloc);
      }
    } else {
      auto relsOrErr = elf.rels(shdr);
      if (!relsOrErr)
        continue;

      for (const auto &rel : *relsOrErr) {
        ELFRelocation reloc;
        reloc.offset = rel.r_offset;
        reloc.type = rel.getType(false);
        reloc.symbolIdx = rel.getSymbol(false);
        reloc.addend = 0;
        relocations[targetSection].push_back(reloc);
      }
    }

    idx++;
  }

  return true;
}

const std::vector<ELFRelocation> &
ELFReader::getRelocations(size_t sectionIdx) const {
  static std::vector<ELFRelocation> empty;
  auto it = relocations.find(sectionIdx);
  if (it != relocations.end())
    return it->second;
  return empty;
}

const ELFSymbol *ELFReader::findSymbol(StringRef name) const {
  for (const auto &sym : symbols) {
    if (sym.name == name)
      return &sym;
  }
  return nullptr;
}

const ELFSection *ELFReader::getTextSection() const {
  for (const auto &sec : sections) {
    if (sec.name == ".text")
      return &sec;
  }
  return nullptr;
}

const ELFSection *ELFReader::getDataSection() const {
  for (const auto &sec : sections) {
    if (sec.name == ".data")
      return &sec;
  }
  return nullptr;
}

const ELFSection *ELFReader::getBSSSection() const {
  for (const auto &sec : sections) {
    if (sec.name == ".bss")
      return &sec;
  }
  return nullptr;
}

} // namespace lld::classic68k
