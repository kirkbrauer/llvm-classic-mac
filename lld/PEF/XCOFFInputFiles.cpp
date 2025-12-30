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
// Helper functions for XCOFF implicit addend extraction
//===----------------------------------------------------------------------===//

/// Read a 32-bit big-endian value from memory
static uint32_t read32BE(const uint8_t *p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
         (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

/// Extract the implicit addend from a D-form instruction (lwz, stw, etc.)
/// These have a 16-bit signed immediate in bits 16-31
static int64_t extractDFormAddend(const uint8_t *instr) {
  uint32_t word = read32BE(instr);
  return static_cast<int16_t>(word & 0xFFFF);  // Sign-extend 16-bit value
}

/// Extract the implicit addend from a branch instruction (bl, b, etc.)
/// These have a 24-bit signed displacement in bits 6-29, shifted left 2
static int64_t extractBranchAddend(const uint8_t *instr) {
  uint32_t word = read32BE(instr);
  // Extract LI field (bits 6-29, which is bits 2-25 of the 26-bit field)
  int32_t offset = (word & 0x03FFFFFC);  // Get LI field with AA and LK bits cleared
  // Sign-extend from 26 bits
  if (offset & 0x02000000)
    offset |= 0xFC000000;
  return offset;
}

/// Extract the implicit addend from a 32-bit absolute address (R_POS)
static int64_t extractAbsoluteAddend(const uint8_t *instr) {
  return read32BE(instr);
}

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
    isec->setFromXCOFF(true);  // Mark as XCOFF for relocation processing
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
  //
  // Additionally, XCOFF data symbols may have paired entries:
  // - A C_HIDEXT (hidden external) TOC pointer slot (just 4 bytes pointing to data)
  // - A C_EXT (external) actual data symbol (the real data struct)
  // We must prefer C_EXT over C_HIDEXT when both have the same name.
  std::set<std::string> addedSymbols;
  std::set<std::string> hiddenSymbols;  // Track which symbols were added as C_HIDEXT

  // Track local C_HIDEXT DATA symbols that need to be updated when C_EXT DATA comes.
  // This handles the case where a CODE symbol with the same name exists in symtab,
  // but we need to update the DATA C_HIDEXT local (not the CODE symbol).
  std::map<std::string, Defined *> hiddenDataLocals;

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

    // Get XCOFF storage class to distinguish C_EXT (global) from C_HIDEXT (hidden TOC entry)
    XCOFFSymbolRef xcoffSym = xcoffObj->toSymbolRef(sym.getRawDataRefImpl());
    XCOFF::StorageClass storageClass = xcoffSym.getStorageClass();
    bool isHiddenExternal = (storageClass == XCOFF::C_HIDEXT);

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
    uint64_t sectionBase = 0;
    bool hasSection = secOrErr && *secOrErr != xcoffObj->section_end();

    if (hasSection) {
      // Find the section index and get section base address
      unsigned idx = 0;
      SectionRef symbolSection = **secOrErr;
      sectionBase = symbolSection.getAddress();
      for (SectionRef sec : xcoffObj->sections()) {
        if (sec == symbolSection) {
          sectionIndex = static_cast<int16_t>(idx);
          break;
        }
        idx++;
      }
    }

    // Convert absolute address to section-relative offset
    // XCOFF symbols have absolute addresses, but we need section offsets for the linker
    if (sectionIndex >= 0 && value >= sectionBase) {
      value = static_cast<uint32_t>(value - sectionBase);
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

      // Check if duplicate - but prefer C_EXT (global) over C_HIDEXT (hidden TOC entry)
      // XCOFF data symbols like __gOTClientRecord appear twice:
      // 1. C_HIDEXT at low offset (TOC pointer slot, just 4 bytes)
      // 2. C_EXT at higher offset (actual data structure)
      // We MUST use the C_EXT version for correct symbol resolution.
      //
      // CRITICAL: XCOFF function symbols come in CODE/DATA pairs:
      // - .FunctionName (CODE in .text) - stripped to FunctionName
      // - FunctionName (DATA in .data) - function descriptor
      // We need BOTH symbols! The CODE symbol is used for intra-module calls,
      // and the DATA symbol is the function descriptor for cross-module calls.
      if (addedSymbols.count(key)) {
        Symbol *existing = symtab->find(cleanName);

        // Determine section types
        bool newIsData = (sectionIndex != 0);
        bool existingIsData = false;
        if (existing && existing->isDefined()) {
          existingIsData = (cast<Defined>(existing)->getSectionIndex() != 0);
        }

        // If the new symbol is C_EXT and the existing one was C_HIDEXT, replace it
        // BUT only if they're in the same section type (both code or both data)!
        if (existing && existing->isDefined() && !isHiddenExternal &&
            hiddenSymbols.count(key)) {
          Defined *existingDef = cast<Defined>(existing);

          if (existingIsData == newIsData) {
            // Same section type - safe to update the hidden symbol
            existingDef->setValue(value);
            existingDef->setOriginalValue(value);
            existingDef->setSectionIndex(sectionIndex);
            importIndexMap[symIndex] = existing;
            hiddenSymbols.erase(key);  // No longer hidden
            hiddenDataLocals.erase(key);  // Also remove from locals map

            if (config->verbose) {
              errorHandler().outs() << "  Updated C_HIDEXT symbol '" << cleanName
                                   << "' with C_EXT definition at 0x"
                                   << utohexstr(value) << " (section " << sectionIndex
                                   << ")\n";
            }
            symIndex++;
            continue;
          }

          // Different section types: Check if we have a DATA C_HIDEXT local to update
          // This happens when CODE symbol is in symtab but DATA C_HIDEXT local exists
          if (newIsData && hiddenDataLocals.count(key)) {
            Defined *dataLocal = hiddenDataLocals[key];
            dataLocal->setValue(value);
            dataLocal->setOriginalValue(value);
            dataLocal->setSectionIndex(sectionIndex);
            importIndexMap[symIndex] = dataLocal;
            hiddenSymbols.erase(key);
            hiddenDataLocals.erase(key);

            // Register in symtab's symVector so it gets VA assignment in Driver.cpp
            // Use a unique name to avoid conflict with the CODE symbol
            std::string dataName = cleanName.str() + "[DS]";  // DS = descriptor
            symtab->registerSymbol(dataName, dataLocal);

            if (config->verbose) {
              errorHandler().outs() << "  Updated DATA C_HIDEXT local '" << cleanName
                                   << "' with C_EXT definition at 0x"
                                   << utohexstr(value) << " (section " << sectionIndex
                                   << ")\n";
            }
            symIndex++;
            continue;
          }

          // NEW: If new is CODE and existing is DATA C_HIDEXT, we need BOTH symbols!
          // The existing DATA symbol will be updated later when C_EXT DATA comes.
          // Add the CODE symbol now - it's needed for intra-module calls.
          if (!newIsData && existingIsData) {
            // This is a CODE symbol that should replace the DATA C_HIDEXT for code calls
            // Update the existing symbol with CODE values
            existingDef->setValue(value);
            existingDef->setOriginalValue(value);
            existingDef->setSectionIndex(sectionIndex);
            importIndexMap[symIndex] = existing;

            // CRITICAL: Remove from hiddenDataLocals since we've repurposed this symbol for CODE.
            // The DATA C_EXT will create a NEW local symbol when it comes.
            hiddenDataLocals.erase(key);
            hiddenSymbols.erase(key);  // No longer a hidden symbol

            if (config->verbose) {
              errorHandler().outs() << "  Replaced DATA C_HIDEXT '" << cleanName
                                   << "' with CODE symbol at 0x"
                                   << utohexstr(value) << " (section " << sectionIndex
                                   << ")\n";
            }
            symIndex++;
            continue;
          }
        }

        // Check if this is a CODE symbol and existing is a DATA symbol (non-hidden)
        // This shouldn't normally happen, but handle it just in case
        if (!newIsData && existingIsData && existing && existing->isDefined()) {
          // Replace the DATA symbol with CODE - CODE is more important for calls
          Defined *existingDef = cast<Defined>(existing);
          existingDef->setValue(value);
          existingDef->setOriginalValue(value);
          existingDef->setSectionIndex(sectionIndex);
          importIndexMap[symIndex] = existing;

          if (config->verbose) {
            errorHandler().outs() << "  Replaced DATA symbol '" << cleanName
                                 << "' with CODE symbol at 0x"
                                 << utohexstr(value) << " (section " << sectionIndex
                                 << ")\n";
          }
          symIndex++;
          continue;
        }

        // DATA C_EXT coming when CODE is already in symtab:
        // This happens when CODE C_EXT replaced the original DATA C_HIDEXT.
        // Create a new local symbol for the function descriptor.
        if (newIsData && !existingIsData && existing && existing->isDefined()) {
          // Create a new local symbol for DATA (function descriptor)
          Defined *dataLocal = make<Defined>(cleanName, this, value, sectionIndex, symbolClass);
          dataLocal->setOriginalValue(value);
          importIndexMap[symIndex] = dataLocal;

          // Register with [DS] suffix so it gets VA assignment
          std::string dataName = cleanName.str() + "[DS]";
          symtab->registerSymbol(dataName, dataLocal);

          if (config->verbose) {
            errorHandler().outs() << "  Created DATA local '" << cleanName
                                 << "' at 0x" << utohexstr(value)
                                 << " (section " << sectionIndex
                                 << ") [CODE already in symtab]\n";
          }
          symIndex++;
          continue;
        }

        // Otherwise, keep existing (original behavior)
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
      if (isHiddenExternal)
        hiddenSymbols.insert(key);  // Track that this symbol came from C_HIDEXT

      if (config->verbose) {
        errorHandler().outs() << "  Defined symbol: " << cleanName << " = 0x"
                              << utohexstr(value) << " (section " << sectionIndex
                              << ")" << (isWeak ? " [weak]" : "")
                              << (isHiddenExternal ? " [C_HIDEXT]" : "") << "\n";
      }
    } else {
      // Local symbol - track for relocation resolution but don't export
      std::string key = cleanName.str();

      // Check if this is a C_HIDEXT local symbol with a name that already exists as C_EXT global
      // If so, use the global symbol's value instead (C_EXT has the correct data offset)
      // BUT only if the existing symbol is in the same section type (both data or both code)!
      // C_HIDEXT TOC pointers (in .data) should use the DATA C_EXT (function descriptor),
      // not the CODE C_EXT (.FunctionName entry point).
      if (isHiddenExternal && addedSymbols.count(key)) {
        Symbol *existing = symtab->find(cleanName);
        if (existing && existing->isDefined()) {
          Defined *existingDef = cast<Defined>(existing);

          // Check if both are in the same section type
          bool existingIsData = (existingDef->getSectionIndex() != 0);
          bool newIsData = (sectionIndex != 0);

          if (existingIsData == newIsData) {
            // Same section type - safe to use existing symbol
            importIndexMap[symIndex] = existing;
            if (config->verbose) {
              errorHandler().outs() << "  C_HIDEXT local '" << cleanName
                                   << "' at 0x" << utohexstr(value)
                                   << " -> using C_EXT global at 0x"
                                   << utohexstr(existingDef->getValue()) << "\n";
            }
            symIndex++;
            continue;
          }
          // Different section types - fall through to create local and wait for data C_EXT
        }
      }

      Defined *localSym = make<Defined>(cleanName, this, value, sectionIndex, symbolClass);
      localSym->setOriginalValue(value);
      importIndexMap[symIndex] = localSym;

      // Track C_HIDEXT locals so global C_EXT can update them later
      // For C_HIDEXT, we need to also add to symtab so that when the C_EXT global
      // comes, symtab->find() will return THIS symbol and we can update its value.
      if (isHiddenExternal) {
        hiddenSymbols.insert(key);

        // If this is a DATA C_HIDEXT, track it in hiddenDataLocals so we can
        // find it even if a CODE symbol with the same name exists in symtab.
        bool isDataSection = (sectionIndex != 0);
        if (isDataSection) {
          hiddenDataLocals[key] = localSym;
        }

        if (!addedSymbols.count(key)) {
          addedSymbols.insert(key);
          // Register in symtab so later C_EXT can find and update it
          symtab->registerSymbol(cleanName, localSym);
        }
        if (config->verbose) {
          errorHandler().outs() << "  Local C_HIDEXT symbol: " << cleanName << " = 0x"
                               << utohexstr(value) << " (section " << sectionIndex
                               << ") [awaiting C_EXT]\n";
        }
      }
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

      // Extract implicit addend from instruction/data
      // XCOFF uses implicit addends encoded in the instruction immediate fields.
      // However, for certain cases the instruction contains placeholder values:
      // 1. Undefined (import) symbols - instruction contains garbage
      // 2. R_TOC relocations - instruction contains placeholder TOC offset that
      //    the linker must compute from scratch based on symbol position
      reloc.addend = 0;
      bool isUndefinedSymbol = reloc.symbol && !reloc.symbol->isDefined();
      bool isTocReloc = (xcoffType == 0x03);  // R_TOC has placeholder, not real addend
      if (!isUndefinedSymbol && !isTocReloc) {
        Expected<ArrayRef<uint8_t>> dataOrErr = isec->getData();
        if (dataOrErr && reloc.offset + 4 <= dataOrErr->size()) {
          const uint8_t *instrPtr = dataOrErr->data() + reloc.offset;
          switch (xcoffType) {
            case 0x00: // R_POS - Absolute address in data
              reloc.addend = extractAbsoluteAddend(instrPtr);
              break;
            case 0x02: // R_REL - Relative
            case 0x0A: // R_BR - Branch
            case 0x1A: // R_RBR - Relative branch
              reloc.addend = extractBranchAddend(instrPtr);
              break;
            // R_TOC (0x03) is excluded above - it has placeholder values
            default:
              // For unknown types, try absolute
              reloc.addend = extractAbsoluteAddend(instrPtr);
              break;
          }
        }
      }

      isec->addELFRelocation(reloc);

      if (config->verbose) {
        StringRef symName = reloc.symbol ? reloc.symbol->getName() : "<unknown>";
        Expected<StringRef> secNameOrErr = sec.getName();
        StringRef secName = secNameOrErr ? *secNameOrErr : "<unknown>";
        errorHandler().outs() << "    Reloc: " << secName << "+0x"
                              << utohexstr(reloc.offset) << " xcoff_type=0x"
                              << utohexstr(xcoffType) << " mapped_type="
                              << reloc.type << " sym=" << symName
                              << " addend=0x" << utohexstr(reloc.addend) << "\n";
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
