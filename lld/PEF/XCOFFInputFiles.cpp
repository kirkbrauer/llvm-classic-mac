//===- XCOFFInputFiles.cpp - XCOFF object file input for PEF linker -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements support for linking XCOFF object files (such as
// OpenTransportAppPPC.o) into PEF executables. These are typically static
// glue libraries from the Mac OS SDK that wrap calls to shared library
// functions.
//
//===----------------------------------------------------------------------===//

#include "InputFiles.h"
#include "Config.h"
#include "InputSection.h"
#include "Symbols.h"
#include "SymbolTable.h"
#include "lld/Common/ErrorHandler.h"
#include "lld/Common/Memory.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/XCOFF.h"
#include "llvm/Object/XCOFFObjectFile.h"

using namespace llvm;
using namespace llvm::object;
using namespace llvm::XCOFF;

namespace lld::pef {

//===----------------------------------------------------------------------===//
// XCOFFObjFile
//===----------------------------------------------------------------------===//

XCOFFObjFile::XCOFFObjFile(MemoryBufferRef m, StringRef archiveName)
    : InputFile(XCOFFObjectKind, m) {
  this->archiveName = std::string(archiveName);
}

void XCOFFObjFile::parse() {
  // Create XCOFF ObjectFile from memory buffer
  Expected<std::unique_ptr<ObjectFile>> objOrErr =
      ObjectFile::createObjectFile(mb);
  if (!objOrErr) {
    error(toString(objOrErr.takeError()) + " in " + getName());
    return;
  }

  // Verify it's an XCOFF file
  if (!isa<XCOFFObjectFile>(*objOrErr)) {
    error(getName() + ": not an XCOFF object file");
    return;
  }

  xcoffObj.reset(cast<XCOFFObjectFile>(objOrErr->release()));

  if (config->verbose) {
    errorHandler().outs() << "Parsing XCOFF object file: " << getName() << "\n";
  }

  // Parse in order: sections, symbols
  parseSections();
  parseSymbols();
  parseRelocations();

  if (config->verbose) {
    errorHandler().outs() << "  Sections: " << inputSections.size() << "\n";
    errorHandler().outs() << "  Symbols: " << symbols.size() << "\n";
  }
}

void XCOFFObjFile::parseSections() {
  unsigned secIndex = 0;
  for (SectionRef sec : xcoffObj->sections()) {
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
        name.starts_with(".debug") ||
        name.starts_with(".info") ||
        name == ".import") {  // Import section handled separately
      secIndex++;
      continue;
    }

    // Get section data
    Expected<StringRef> contentsOrErr = sec.getContents();
    ArrayRef<uint8_t> data;
    if (contentsOrErr) {
      data = ArrayRef<uint8_t>(
          reinterpret_cast<const uint8_t *>(contentsOrErr->data()),
          contentsOrErr->size());
    } else {
      // BSS sections have no contents
      consumeError(contentsOrErr.takeError());
    }

    // Skip empty sections (unless BSS)
    bool isBss = sec.isBSS();
    if (data.empty() && !isBss) {
      secIndex++;
      continue;
    }

    // Determine PEF section kind based on XCOFF section properties
    uint8_t pefKind = 1; // Default to data
    if (sec.isText()) {
      pefKind = 0; // Code section
    } else if (sec.isData() || isBss) {
      pefKind = 1; // Unpacked data section
    }

    // XCOFF doesn't expose alignment via LLVM's generic interface
    // (getSectionAlignment is not implemented for XCOFF)
    // Default to 4-byte alignment for PowerPC code/data
    uint32_t alignment = 4;

    // Handle BSS sections - create zero-filled data
    std::vector<uint8_t> bssData;
    if (isBss && sec.getSize() > 0) {
      bssData.resize(sec.getSize(), 0);
      data = ArrayRef<uint8_t>(bssData);
    }

    // Create InputSection for the PEF writer
    auto *isec = make<InputSection>(this, secIndex, name, pefKind, data, alignment);
    inputSections.push_back(isec);
    sectionMap[secIndex] = isec;

    if (config->verbose) {
      errorHandler().outs() << "  Section " << secIndex << ": " << name
                            << " size=0x" << utohexstr(sec.getSize())
                            << " kind=" << (int)pefKind << "\n";
    }

    secIndex++;
  }
}

void XCOFFObjFile::parseSymbols() {
  // Track which symbols we've already added to avoid duplicates.
  // XCOFF has paired symbols: .function (code) and function (TOC entry).
  // We want the code symbol (.function stripped to function) and skip the
  // TOC entry (function) to avoid duplicates.
  std::set<std::string> addedSymbols;

  uint32_t symIndex = 0;
  for (SymbolRef sym : xcoffObj->symbols()) {
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

    // Skip file symbols
    if (type == SymbolRef::ST_File) {
      symIndex++;
      continue;
    }

    // Check symbol binding
    uint32_t flags = cantFail(sym.getFlags());
    bool isUndefined = (flags & SymbolRef::SF_Undefined);
    bool isGlobal = (flags & SymbolRef::SF_Global);
    bool isWeak = (flags & SymbolRef::SF_Weak);

    // XCOFF function symbols come in pairs:
    // - .FunctionName (in .text) - the actual code entry point
    // - FunctionName (in .data) - the TOC entry / function descriptor
    //
    // For PEF linking, we need the code symbol. We prefer symbols with
    // the leading dot (code symbols) over those without (TOC entries).
    // When we see a .function symbol, we add it as "function".
    // When we see a function symbol (no dot), we skip it if we already
    // have the .function version.
    bool hasDotPrefix = name.starts_with(".");
    StringRef cleanName = hasDotPrefix ? name.drop_front(1) : name;

    if (cleanName.empty()) {
      symIndex++;
      continue;
    }

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
    if (secOrErr && *secOrErr != xcoffObj->section_end()) {
      // Find the section index
      unsigned idx = 0;
      for (SectionRef sec : xcoffObj->sections()) {
        if (sec == **secOrErr) {
          sectionIndex = static_cast<int16_t>(idx);
          break;
        }
        idx++;
      }
    }

    // Determine PEF symbol class based on whether this is a code or data symbol
    // Symbols with . prefix are code entry points
    uint8_t symbolClass = hasDotPrefix ? 0 : 1; // 0=code, 1=data

    // Handle defined vs undefined symbols
    if (isUndefined) {
      // Skip CFM private runtime symbols - these are resolved by CFM at load time
      // and are not part of any public shared library. They include:
      // - InitLibraryManager, CleanupLibraryManager (library loading)
      // - LoadClass, UnloadClass (class loading)
      // - SetCurrentClient, SetSelfAsClient (client context)
      // - FragGetSectionInfo (section queries)
      // - _ptrgl* (PPC glue routines)
      // - __dl__FPv (deallocation stub)
      if (cleanName == "InitLibraryManager" ||
          cleanName == "CleanupLibraryManager" ||
          cleanName == "FragGetSectionInfo" ||
          cleanName == "LoadClass" ||
          cleanName == "UnloadClass" ||
          cleanName == "SetCurrentClient" ||
          cleanName == "SetSelfAsClient" ||
          cleanName.starts_with("_ptrgl") ||
          cleanName == "__dl__FPv") {
        if (config->verbose) {
          errorHandler().outs() << "  Skipping CFM runtime symbol: " << cleanName << "\n";
        }
        symIndex++;
        continue;
      }

      // Undefined symbol - add to symbol table as undefined
      // Skip if already added
      std::string key = cleanName.str();
      if (addedSymbols.count(key)) {
        // Already have this symbol, just map the index
        Symbol *existing = symtab->find(cleanName);
        if (existing)
          importIndexMap[symIndex] = existing;
        symIndex++;
        continue;
      }

      Symbol *undefinedSym = symtab->addUndefined(cleanName, this);
      if (!undefinedSym) {
        // Symbol already defined elsewhere - look it up
        undefinedSym = symtab->find(cleanName);
      }
      if (undefinedSym) {
        symbols.push_back(undefinedSym);
        importIndexMap[symIndex] = undefinedSym;
        addedSymbols.insert(key);
      }

      if (config->verbose) {
        errorHandler().outs() << "  Undefined symbol: " << cleanName << "\n";
      }
    } else if (isGlobal || isWeak) {
      // Defined global/weak symbol
      std::string key = cleanName.str();

      // Skip if already added (prefer the .function version which comes first)
      if (addedSymbols.count(key)) {
        // Already have this symbol, just map the index to existing
        Symbol *existing = symtab->find(cleanName);
        if (existing)
          importIndexMap[symIndex] = existing;
        symIndex++;
        continue;
      }

      Defined *defSym =
          symtab->addDefined(cleanName, this, value, sectionIndex, symbolClass, isWeak);
      defSym->setOriginalValue(value);
      symbols.push_back(defSym);
      importIndexMap[symIndex] = defSym;
      addedSymbols.insert(key);

      if (config->verbose) {
        errorHandler().outs() << "  Defined symbol: " << cleanName << " = 0x"
                              << utohexstr(value) << " (section " << sectionIndex
                              << ")" << (isWeak ? " [weak]" : "") << "\n";
      }
    } else {
      // Local symbol - track for relocation resolution but don't export
      Defined *localSym = make<Defined>(cleanName, this, value, sectionIndex, symbolClass);
      localSym->setOriginalValue(value);
      importIndexMap[symIndex] = localSym;
    }

    symIndex++;
  }
}

void XCOFFObjFile::parseRelocations() {
  // Build symbol index lookup for relocations
  std::map<DataRefImpl, uint32_t> symIterToIndex;
  uint32_t symIdx = 0;
  for (SymbolRef sym : xcoffObj->symbols()) {
    symIterToIndex[sym.getRawDataRefImpl()] = symIdx++;
  }

  // Process relocations for each section
  unsigned secIndex = 0;
  for (SectionRef sec : xcoffObj->sections()) {
    // Find corresponding InputSection
    auto it = sectionMap.find(secIndex);
    if (it == sectionMap.end()) {
      secIndex++;
      continue;
    }
    InputSection *isec = it->second;

    // Get relocations for this section
    for (RelocationRef rel : sec.relocations()) {
      InputSectionReloc reloc;
      reloc.offset = rel.getOffset();

      // Map XCOFF relocation type to ELF-compatible type for our writer
      // XCOFF uses different relocation type numbering than ELF
      uint64_t xcoffType = rel.getType();

      // Map common XCOFF relocation types:
      // R_POS (0x00) - Positive relocation -> R_PPC_ADDR32 (1)
      // R_NEG (0x01) - Negative relocation
      // R_REL (0x02) - Relative -> R_PPC_REL24 (10)
      // R_TOC (0x03) - TOC-relative -> R_PPC_ADDR16_LO/HI (4/5)
      // R_TRL (0x12) - TOC reload -> skip
      // R_TRLA (0x13) - TOC reload adjust -> skip
      // R_BR (0x0A) - Branch relative -> R_PPC_REL24 (10)
      // R_RBR (0x1A) - Relative branch -> R_PPC_REL24 (10)
      switch (xcoffType) {
        case 0x00: // R_POS - Absolute address
          reloc.type = 1; // R_PPC_ADDR32
          break;
        case 0x02: // R_REL - Relative
        case 0x0A: // R_BR - Branch
        case 0x1A: // R_RBR - Relative branch
          reloc.type = 10; // R_PPC_REL24
          break;
        case 0x03: // R_TOC - TOC relative
          reloc.type = 4; // R_PPC_ADDR16_LO (we'll fix this during processing)
          break;
        default:
          // For unknown types, treat as absolute
          reloc.type = 1;
          break;
      }

      // Get the symbol for this relocation
      symbol_iterator symIt = rel.getSymbol();
      reloc.symbol = nullptr;

      if (symIt != xcoffObj->symbol_end()) {
        auto indexIt = symIterToIndex.find(symIt->getRawDataRefImpl());
        if (indexIt != symIterToIndex.end()) {
          auto symMapIt = importIndexMap.find(indexIt->second);
          if (symMapIt != importIndexMap.end()) {
            reloc.symbol = symMapIt->second;
          }
        }
      }

      // XCOFF doesn't have explicit addends like ELF RELA
      reloc.addend = 0;

      isec->addELFRelocation(reloc);

      if (config->verbose) {
        StringRef symName = reloc.symbol ? reloc.symbol->getName() : "<unknown>";
        Expected<StringRef> secNameOrErr = sec.getName();
        StringRef secName = secNameOrErr ? *secNameOrErr : "<unknown>";
        errorHandler().outs() << "    Reloc: " << secName << "+0x"
                              << utohexstr(reloc.offset) << " xcoff_type=0x"
                              << utohexstr(xcoffType) << " mapped_type="
                              << reloc.type << " sym=" << symName << "\n";
      }
    }

    secIndex++;
  }
}

//===----------------------------------------------------------------------===//
// Factory function
//===----------------------------------------------------------------------===//

InputFile *createXCOFFObjectFile(MemoryBufferRef mb, StringRef archiveName) {
  auto *file = make<XCOFFObjFile>(mb, archiveName);
  file->parse();
  return file;
}

} // namespace lld::pef
