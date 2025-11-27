//===- ELFInputFiles.cpp - ELF object file input for PEF linker -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ELFInputFiles.h"
#include "Config.h"
#include "InputSection.h"
#include "Symbols.h"
#include "SymbolTable.h"
#include "lld/Common/ErrorHandler.h"
#include "lld/Common/Memory.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELFObjectFile.h"

using namespace llvm;
using namespace llvm::object;
using namespace llvm::ELF;

namespace lld::pef {

//===----------------------------------------------------------------------===//
// ELFInputSection
//===----------------------------------------------------------------------===//

ELFInputSection::ELFInputSection(ELFObjFile *file, unsigned index,
                                 SectionRef secRef)
    : file(file), sectionIndex(index) {
  // Get section name
  if (Expected<StringRef> nameOrErr = secRef.getName()) {
    name = *nameOrErr;
  } else {
    consumeError(nameOrErr.takeError());
    name = "";
  }

  // Get section size
  size = secRef.getSize();

  // Get section data
  // Note: BSS sections (SHT_NOBITS) have no file content but have a size
  // We need to handle them specially - check if this is a BSS section FIRST
  bool isBss = (name == ".bss" || name.starts_with(".bss.") ||
                name == ".sbss" || name.starts_with(".sbss."));

  if (isBss && size > 0) {
    // BSS sections get zero-filled data
    bssData.resize(size, 0);
    data = ArrayRef<uint8_t>(bssData);
  } else if (Expected<StringRef> dataOrErr = secRef.getContents()) {
    data = ArrayRef<uint8_t>(
        reinterpret_cast<const uint8_t *>(dataOrErr->data()), dataOrErr->size());
  } else {
    consumeError(dataOrErr.takeError());
    data = ArrayRef<uint8_t>();
  }

  // Get alignment
  alignment = secRef.getAlignment().value();
  if (alignment == 0)
    alignment = 1;

  // Determine PEF section kind based on ELF section properties
  // Use section name heuristics
  //
  // Note: .rodata sections are placed in the data section (kind=1) rather than
  // constant section (kind=3) because PowerPC code accesses global data using
  // TOC-relative addressing (addi rX, r2, offset), and the TOC pointer (r2)
  // points to the data section. If .rodata were in a separate constant section,
  // TOC-relative addressing would fail.
  if (name == ".text" || name.starts_with(".text.")) {
    pefKind = 0; // Code section
  } else if (name == ".data" || name.starts_with(".data.") ||
             name == ".sdata" || name.starts_with(".sdata.") ||
             name == ".rodata" || name.starts_with(".rodata.") ||
             name == ".sdata2" || name.starts_with(".sdata2.") ||
             name == ".bss" || name.starts_with(".bss.") ||
             name == ".sbss" || name.starts_with(".sbss.")) {
    pefKind = 1; // Unpacked data section (includes read-only data for TOC access)
  } else {
    // Default to data for unknown sections
    pefKind = 1;
  }
}

//===----------------------------------------------------------------------===//
// ELFObjFile
//===----------------------------------------------------------------------===//

ELFObjFile::ELFObjFile(MemoryBufferRef m, StringRef archiveName)
    : InputFile(ELFObjectKind, m) {
  this->archiveName = std::string(archiveName);
}

void ELFObjFile::parse() {
  // Create ELF ObjectFile from memory buffer
  Expected<std::unique_ptr<ObjectFile>> objOrErr =
      ObjectFile::createELFObjectFile(mb);
  if (!objOrErr) {
    error(toString(objOrErr.takeError()) + " in " + getName());
    return;
  }

  elfObj = std::move(*objOrErr);

  // Verify it's a PowerPC ELF file
  if (elfObj->getArch() != Triple::ppc) {
    error(getName() + ": not a PowerPC ELF file (arch=" +
          Triple::getArchTypeName(elfObj->getArch()) + ")");
    return;
  }

  if (config->verbose) {
    errorHandler().outs() << "Parsing ELF object file: " << getName() << "\n";
  }

  // Parse in order: sections, symbols, relocations
  parseSections();
  parseSymbols();
  parseRelocations();

  // Copy relocations from ELFInputSection to InputSection wrappers
  for (auto &[secIdx, elfIsec] : sectionMap) {
    auto it = inputSectionMap.find(secIdx);
    if (it == inputSectionMap.end())
      continue;
    InputSection *isec = it->second;

    size_t relocCount = elfIsec->getRelocations().size();
    if (config->verbose && relocCount > 0) {
      errorHandler().outs() << "  Copying " << relocCount << " relocations to InputSection '"
                            << elfIsec->getName() << "'\n";
    }

    for (const ELFRelocation &elfRel : elfIsec->getRelocations()) {
      InputSectionReloc reloc;
      reloc.offset = elfRel.offset;
      reloc.type = elfRel.type;
      reloc.symbol = elfRel.symbol;
      reloc.addend = elfRel.addend;
      isec->addELFRelocation(reloc);
    }
  }

  if (config->verbose) {
    errorHandler().outs() << "  Sections: " << elfInputSections.size() << "\n";
    errorHandler().outs() << "  Symbols: " << symbols.size() << "\n";
  }
}

void ELFObjFile::parseSections() {
  unsigned secIndex = 0;
  for (SectionRef sec : elfObj->sections()) {
    // Get section name
    Expected<StringRef> nameOrErr = sec.getName();
    if (!nameOrErr) {
      consumeError(nameOrErr.takeError());
      secIndex++;
      continue;
    }
    StringRef name = *nameOrErr;

    // Skip certain sections that aren't needed for PEF
    if (name.empty() ||
        name.starts_with(".note") ||
        name.starts_with(".comment") ||
        name.starts_with(".llvm") ||
        name.starts_with(".debug") ||
        name.starts_with(".eh_frame") ||
        name.starts_with(".gcc_except") ||
        name == ".symtab" ||
        name == ".strtab" ||
        name == ".shstrtab" ||
        name.starts_with(".rela") ||
        name.starts_with(".rel.")) {
      secIndex++;
      continue;
    }

    // Skip empty sections
    if (sec.getSize() == 0) {
      secIndex++;
      continue;
    }

    // Create ELFInputSection (for relocation processing)
    auto *elfIsec = make<ELFInputSection>(this, secIndex, sec);
    elfInputSections.push_back(elfIsec);
    sectionMap[secIndex] = elfIsec;

    // Create corresponding InputSection (for Writer.cpp compatibility)
    auto *isec = make<InputSection>(this, secIndex, elfIsec->getName(),
                                     elfIsec->getPEFKind(), elfIsec->getData(),
                                     elfIsec->getAlignment());
    inputSections.push_back(isec);
    inputSectionMap[secIndex] = isec;

    if (config->verbose) {
      errorHandler().outs() << "  Section " << secIndex << ": " << name
                            << " size=0x" << utohexstr(elfIsec->getSize())
                            << " kind=" << (int)elfIsec->getPEFKind() << "\n";
    }

    secIndex++;
  }
}

void ELFObjFile::parseSymbols() {
  uint32_t symIndex = 0;
  for (SymbolRef sym : elfObj->symbols()) {
    // Get symbol name
    Expected<StringRef> nameOrErr = sym.getName();
    if (!nameOrErr) {
      consumeError(nameOrErr.takeError());
      symIndex++;
      continue;
    }
    StringRef name = *nameOrErr;

    // Skip unnamed symbols
    if (name.empty()) {
      symIndex++;
      continue;
    }

    // Get symbol type
    Expected<SymbolRef::Type> typeOrErr = sym.getType();
    SymbolRef::Type type = typeOrErr ? *typeOrErr : SymbolRef::ST_Unknown;

    // Skip file symbols (we don't need source file names)
    if (type == SymbolRef::ST_File) {
      symIndex++;
      continue;
    }

    // Section symbols (STT_SECTION) - LLVM maps these to ST_Debug
    // These are used internally by the assembler for section-relative relocations
    // We need to track them locally for relocation resolution, but not in global symtab
    bool isSectionSymbol = (type == SymbolRef::ST_Debug && name.starts_with("."));

    // Get symbol value
    Expected<uint64_t> addrOrErr = sym.getAddress();
    if (!addrOrErr) {
      consumeError(addrOrErr.takeError());
      symIndex++;
      continue;
    }
    uint32_t value = static_cast<uint32_t>(*addrOrErr);

    // Get symbol section
    Expected<section_iterator> secOrErr = sym.getSection();
    int16_t sectionIndex = -1;
    if (secOrErr && *secOrErr != elfObj->section_end()) {
      // Find the section index
      unsigned idx = 0;
      for (SectionRef sec : elfObj->sections()) {
        if (sec == **secOrErr) {
          sectionIndex = static_cast<int16_t>(idx);
          break;
        }
        idx++;
      }
    }

    // Check symbol binding
    uint32_t flags = cantFail(sym.getFlags());
    bool isUndefined = (flags & SymbolRef::SF_Undefined);
    bool isGlobal = (flags & SymbolRef::SF_Global);
    bool isWeak = (flags & SymbolRef::SF_Weak);

    // Determine PEF symbol class
    uint8_t symbolClass = 0; // Default to code
    if (type == SymbolRef::ST_Data || type == SymbolRef::ST_Other) {
      symbolClass = 1; // Data symbol
    }

    // Handle defined vs undefined symbols
    if (isUndefined) {
      // Undefined symbol - add to symbol table as undefined
      // Note: addUndefined returns nullptr if symbol is already defined elsewhere
      Symbol *sym = symtab->addUndefined(name, this);
      if (!sym) {
        // Symbol already defined - look it up to get the Defined symbol
        sym = symtab->find(name);
      }
      if (sym) {
        symbols.push_back(sym);
        elfSymbolMap[symIndex] = sym;
      }

      if (config->verbose) {
        errorHandler().outs() << "  Undefined symbol: " << name << "\n";
      }
    } else if (isGlobal || isWeak) {
      // Defined global/weak symbol
      Defined *defSym =
          symtab->addDefined(name, this, value, sectionIndex, symbolClass);
      defSym->setOriginalValue(value);
      symbols.push_back(defSym);
      elfSymbolMap[symIndex] = defSym;

      if (config->verbose) {
        errorHandler().outs() << "  Defined symbol: " << name << " = 0x"
                              << utohexstr(value) << " (section " << sectionIndex
                              << ")\n";
      }
    } else if (isSectionSymbol) {
      // Section symbol - track locally only for relocation resolution
      // Don't add to global symbol table to avoid conflicts between files
      // Create a file-local Defined symbol (not through symtab)
      Defined *localSym = make<Defined>(name, this, value, sectionIndex, symbolClass);
      localSym->setOriginalValue(value);
      elfSymbolMap[symIndex] = localSym;
    } else {
      // Local symbol - track in map but don't export
      // Create a local symbol entry for relocation resolution
      Defined *localSym =
          symtab->addDefined(name, this, value, sectionIndex, symbolClass);
      localSym->setOriginalValue(value);
      elfSymbolMap[symIndex] = localSym;
    }

    symIndex++;
  }
}

void ELFObjFile::parseRelocations() {
  // Iterate through all sections looking for relocation sections
  for (SectionRef sec : elfObj->sections()) {
    Expected<StringRef> nameOrErr = sec.getName();
    if (!nameOrErr) {
      consumeError(nameOrErr.takeError());
      continue;
    }
    StringRef name = *nameOrErr;

    // Check if this is a relocation section
    if (!name.starts_with(".rela") && !name.starts_with(".rel."))
      continue;

    // Get the target section for these relocations
    // .rela.text -> .text, .rel.data -> .data
    StringRef targetName;
    if (name.starts_with(".rela."))
      targetName = name.drop_front(5);  // Drop ".rela" to get ".text" from ".rela.text"
    else if (name.starts_with(".rela"))
      targetName = name.drop_front(4);  // Drop ".rel" to get "a.text" from ".relatext" (unlikely case)
    else if (name.starts_with(".rel."))
      targetName = name.drop_front(4);  // Drop ".rel" to get ".data" from ".rel.data"
    else
      targetName = name.drop_front(4);  // Fallback

    // Find the target section in our section map
    ELFInputSection *targetSec = nullptr;
    for (auto &[idx, isec] : sectionMap) {
      if (isec->getName() == targetName) {
        targetSec = isec;
        break;
      }
    }

    if (!targetSec) {
      // Target section might have been skipped
      continue;
    }

    // Parse relocations
    // First, build a map from symbol_iterator to index for fast lookup
    std::map<DataRefImpl, uint32_t> symIterToIndex;
    uint32_t symIdx = 0;
    for (SymbolRef sym : elfObj->symbols()) {
      symIterToIndex[sym.getRawDataRefImpl()] = symIdx++;
    }

    for (RelocationRef rel : sec.relocations()) {
      ELFRelocation elfRel;
      elfRel.offset = rel.getOffset();
      elfRel.type = rel.getType();

      // Get the symbol for this relocation
      symbol_iterator symIt = rel.getSymbol();
      elfRel.symbol = nullptr;
      elfRel.symbolIndex = 0;

      if (symIt != elfObj->symbol_end()) {
        // Get symbol index from the iterator
        auto indexIt = symIterToIndex.find(symIt->getRawDataRefImpl());
        if (indexIt != symIterToIndex.end()) {
          elfRel.symbolIndex = indexIt->second;
          // First try to find in our local elfSymbolMap (includes section symbols)
          auto localIt = elfSymbolMap.find(elfRel.symbolIndex);
          if (localIt != elfSymbolMap.end()) {
            elfRel.symbol = localIt->second;
          } else {
            // Fallback to global symbol table lookup by name
            Expected<StringRef> symNameOrErr = symIt->getName();
            if (symNameOrErr) {
              elfRel.symbol = symtab->find(*symNameOrErr);
            } else {
              consumeError(symNameOrErr.takeError());
            }
          }
        }
      }

      // Get addend - for RELA sections, use the relocation's stored addend
      // Cast to ELFRelocationRef to access getAddend()
      ELFRelocationRef elfRelRef(rel);
      if (Expected<int64_t> addendOrErr = elfRelRef.getAddend()) {
        elfRel.addend = *addendOrErr;
      } else {
        // REL sections don't have explicit addends - consume the error
        consumeError(addendOrErr.takeError());
        elfRel.addend = 0;
      }

      targetSec->addRelocation(elfRel);

      if (config->verbose) {
        StringRef symName = elfRel.symbol ? elfRel.symbol->getName() : "<unknown>";
        errorHandler().outs() << "    Reloc: " << targetName << "+0x"
                              << utohexstr(elfRel.offset) << " type="
                              << elfRel.type << " sym=" << symName
                              << " addend=" << elfRel.addend << "\n";
      }
    }
  }
}

uint8_t ELFObjFile::mapELFSectionToPEFKind(uint32_t type, uint64_t flags) const {
  // SHT_PROGBITS with SHF_EXECINSTR -> code
  if ((flags & SHF_EXECINSTR) != 0)
    return 0; // Code

  // SHT_PROGBITS with SHF_WRITE -> data
  if ((flags & SHF_WRITE) != 0)
    return 1; // Unpacked data

  // SHT_PROGBITS read-only -> constant
  if ((flags & SHF_ALLOC) != 0 && (flags & SHF_WRITE) == 0)
    return 3; // Constant

  // SHT_NOBITS (BSS) -> data
  if (type == SHT_NOBITS)
    return 1; // Unpacked data (zero-initialized)

  // Default to data
  return 1;
}

//===----------------------------------------------------------------------===//
// Factory function
//===----------------------------------------------------------------------===//

ELFObjFile *createELFObjectFile(MemoryBufferRef mb, StringRef archiveName) {
  auto *file = make<ELFObjFile>(mb, archiveName);
  file->parse();
  return file;
}

} // namespace lld::pef
