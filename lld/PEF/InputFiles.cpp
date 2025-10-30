//===- InputFiles.cpp -----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "InputFiles.h"
#include "Config.h"
#include "InputSection.h"
#include "SymbolTable.h"
#include "lld/Common/ErrorHandler.h"
#include "lld/Common/Memory.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/Magic.h"
#include "llvm/BinaryFormat/PEF.h"
#include "llvm/Object/PEFObjectFile.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

using namespace llvm;
using namespace llvm::object;
using namespace llvm::sys;

namespace lld::pef {

// Opens a file and returns a MemoryBufferRef
std::optional<MemoryBufferRef> readFile(StringRef path) {
  // Check if file exists
  if (!fs::exists(path)) {
    error("cannot open " + path + ": No such file or directory");
    return std::nullopt;
  }

  // Open the file
  auto mbOrErr = MemoryBuffer::getFile(path, /*IsText=*/false,
                                       /*RequiresNullTerminator=*/false);
  if (auto ec = mbOrErr.getError()) {
    error("cannot open " + path + ": " + ec.message());
    return std::nullopt;
  }

  std::unique_ptr<MemoryBuffer> &mb = *mbOrErr;
  MemoryBufferRef mbref = mb->getMemBufferRef();
  make<std::unique_ptr<MemoryBuffer>>(std::move(mb)); // Take ownership

  return mbref;
}

// Constructor for ObjFile
ObjFile::ObjFile(MemoryBufferRef m, StringRef archiveName)
    : InputFile(ObjectKind, m) {
  this->archiveName = std::string(archiveName);
}

// Parse a PEF object file
void ObjFile::parse() {
  // Create PEFObjectFile from memory buffer
  auto objOrErr = PEFObjectFile::create(mb);
  if (!objOrErr) {
    error(toString(objOrErr.takeError()) + " in " + getName());
    return;
  }

  pefObj = std::move(*objOrErr);

  if (config->verbose) {
    errorHandler().outs() << "Parsing PEF object file: " << getName() << "\n";
    errorHandler().outs() << "  Sections: " << pefObj->getSectionCount() << "\n";
  }

  // Phase 1.4 - Create InputSection objects for each section
  for (unsigned i = 0; i < pefObj->getSectionCount(); ++i) {
    auto hdrOrErr = pefObj->getSectionHeader(i);
    if (!hdrOrErr) {
      error(toString(hdrOrErr.takeError()) + " in " + getName());
      continue;
    }

    // Skip loader section - it's not part of the output
    if (hdrOrErr->SectionKind == PEF::kPEFLoaderSection)
      continue;

    auto *isec = make<InputSection>(this, i, *hdrOrErr);
    inputSections.push_back(isec);

    if (config->verbose) {
      errorHandler().outs() << "  Section " << i << ": "
                           << isec->getName()
                           << " size=0x" << utohexstr(isec->getSize())
                           << " kind=" << (int)isec->getKind() << "\n";
    }
  }

  // Phase 1.3 - Extract exported symbols
  // Iterate through all symbols in the object file using ObjectFile's symbol iterator
  for (SymbolRef sym : pefObj->symbols()) {
    auto nameOrErr = sym.getName();
    if (!nameOrErr) {
      error(toString(nameOrErr.takeError()) + " in " + getName());
      continue;
    }

    StringRef name = *nameOrErr;
    if (name.empty())
      continue;

    auto addrOrErr = sym.getAddress();
    if (!addrOrErr) {
      error(toString(addrOrErr.takeError()) + " in " + getName());
      continue;
    }

    uint64_t addr = *addrOrErr;
    uint32_t value = static_cast<uint32_t>(addr);

    // Get section index
    auto secOrErr = sym.getSection();
    int16_t sectionIndex = -1;
    if (secOrErr && *secOrErr != pefObj->section_end()) {
      sectionIndex = static_cast<int16_t>((*secOrErr)->getIndex());
    }

    // For PEF, we use the symbol flags to determine class
    // This is a simplified approach - real PEF has more complex symbol classes
    auto flagsOrErr = sym.getFlags();
    uint8_t symbolClass = 0; // Default to code
    if (flagsOrErr) {
      uint32_t flags = *flagsOrErr;
      // Map SymbolRef flags to PEF symbol class
      auto typeOrErr = sym.getType();
      if (typeOrErr && *typeOrErr == SymbolRef::ST_Data) {
        symbolClass = 1; // Data symbol
      }
    }

    // Add to symbol table
    auto *definedSym = symtab->addDefined(name, this, value, sectionIndex, symbolClass);
    symbols.push_back(definedSym);
  }

  // Phase 3 - Read relocations from loader section
  auto loaderInfoOrErr = pefObj->getLoaderInfoHeader();
  if (loaderInfoOrErr) {
    const PEF::LoaderInfoHeader &loaderInfo = *loaderInfoOrErr;

    if (config->verbose && loaderInfo.RelocSectionCount > 0) {
      errorHandler().outs() << "  Reading " << loaderInfo.RelocSectionCount
                           << " relocation sections\n";
    }

    // Read relocation headers (one per section with relocations)
    for (unsigned i = 0; i < loaderInfo.RelocSectionCount; ++i) {
      uint64_t headerOffset = loaderInfo.RelocInstrOffset + i * 12;
      auto relocHdrOrErr = pefObj->getRelocHeader(headerOffset);
      if (!relocHdrOrErr) {
        error("failed to read relocation header: " +
              toString(relocHdrOrErr.takeError()) + " in " + getName());
        continue;
      }

      const PEF::LoaderRelocationHeader &relocHdr = *relocHdrOrErr;

      // Read relocation instructions for this section
      auto relocInstrsOrErr =
          pefObj->getRelocInstructions(relocHdr.FirstRelocOffset,
                                       relocHdr.RelocCount);
      if (!relocInstrsOrErr) {
        error("failed to read relocation instructions: " +
              toString(relocInstrsOrErr.takeError()) + " in " + getName());
        continue;
      }

      // Store in InputSection for later processing
      if (relocHdr.SectionIndex < inputSections.size()) {
        InputSection *isec = inputSections[relocHdr.SectionIndex];
        isec->setRelocations(*relocInstrsOrErr);

        if (config->verbose) {
          errorHandler().outs()
              << "    Section " << relocHdr.SectionIndex << " has "
              << relocHdr.RelocCount << " relocation instructions\n";
        }

        // Phase 3.2 - Extract undefined symbols from import relocations
        ArrayRef<uint16_t> relocs = *relocInstrsOrErr;
        for (size_t j = 0; j < relocs.size(); ) {
          uint16_t instr = support::endian::read16be(&relocs[j]);
          uint8_t opcode = (instr >> 10) & 0x3F;
          uint16_t operand = instr & 0x3FF;

          switch (opcode) {
            case PEF::kPEFRelocSmByImport: {
              // Small import reference (index in operand)
              uint32_t importIndex = operand;
              auto symNameOrErr = pefObj->getImportedSymbolName(importIndex);
              if (symNameOrErr) {
                StringRef symName = *symNameOrErr;
                symtab->addUndefined(symName, this);

                if (config->verbose) {
                  errorHandler().outs() << "      Import reference: "
                                       << symName << " (index " << importIndex
                                       << ")\n";
                }
              }
              break;
            }

            case PEF::kPEFRelocLgByImport: {
              // Large import reference (2 instructions)
              uint32_t importIndex = (operand << 16);
              if (j + 1 < relocs.size()) {
                uint16_t instr2 = support::endian::read16be(&relocs[j + 1]);
                importIndex |= instr2;
                j++; // Skip second instruction

                auto symNameOrErr = pefObj->getImportedSymbolName(importIndex);
                if (symNameOrErr) {
                  StringRef symName = *symNameOrErr;
                  symtab->addUndefined(symName, this);

                  if (config->verbose) {
                    errorHandler().outs()
                        << "      Import reference (large): " << symName
                        << " (index " << importIndex << ")\n";
                  }
                }
              }
              break;
            }

            case PEF::kPEFRelocSetPosition: {
              // SetPosition uses 2 instructions - skip second
              if (j + 1 < relocs.size())
                j++;
              break;
            }

            // Other opcodes don't reference imports
            default:
              break;
          }

          j++;
        }
      } else {
        error("relocation header references invalid section index " +
              Twine(relocHdr.SectionIndex) + " in " + getName());
      }
    }
  }

  // Phase 2 - Handle imported symbols
  // For object files created by our compiler, imported symbols are tracked
  // in the ImportedSymbols vector in the PEF object file's loader section.
  // However, the real import resolution happens when we link multiple object
  // files together - undefined symbol references across object files become imports.
  //
  // The key insight: In PEF object files (not executables), imports are really
  // just undefined symbols that will be resolved either by:
  // 1. Other object files being linked together, OR
  // 2. Shared libraries (InterfaceLib, MathLib, etc.)
  //
  // So the linker's job in Phase 2 is to:
  // 1. Collect all undefined symbols from all input files
  // 2. Try to resolve them against other input files first
  // 3. Any remaining undefined symbols become imports from shared libraries
  //
  // This is handled later in the linking process, not here in the object file reader.

  if (config->verbose) {
    errorHandler().outs() << "  Defined symbols: " << symbols.size() << "\n";
  }
}

// Create an object file from a memory buffer
InputFile *createObjectFile(MemoryBufferRef mb, StringRef archiveName) {
  // Identify the file type
  file_magic magic = identify_magic(mb.getBuffer());

  // Check if it's a PEF file
  if (magic != file_magic::pef_object) {
    error(mb.getBufferIdentifier() + ": unknown file type");
    return nullptr;
  }

  // Create and parse the object file
  auto *file = make<ObjFile>(mb, archiveName);
  file->parse();
  return file;
}

//===----------------------------------------------------------------------===//
// SharedLibraryFile - Phase 2
//===----------------------------------------------------------------------===//

// Constructor for SharedLibraryFile
SharedLibraryFile::SharedLibraryFile(MemoryBufferRef m, bool isWeak)
    : InputFile(SharedLibraryKind, m), weak(isWeak) {
  // Extract library name from filename (without extension)
  StringRef filename = path::filename(getName());
  StringRef stem = path::stem(filename);
  libraryName = std::string(stem);
}

// Parse a PEF shared library and extract exported symbols
void SharedLibraryFile::parse() {
  // Create PEFObjectFile from memory buffer
  auto objOrErr = PEFObjectFile::create(mb);
  if (!objOrErr) {
    error(toString(objOrErr.takeError()) + " in " + getName());
    return;
  }

  pefLib = std::move(*objOrErr);

  if (config->verbose) {
    errorHandler().outs() << "Parsing PEF shared library: " << getName()
                          << " (" << libraryName << ")\n";
  }

  // Extract exported symbols from loader section
  // Phase 2: We'll read the export table and create symbols
  // For now, just register the library with the symbol table
  // The actual symbol resolution will happen in Driver.cpp

  // Try to get the loader section
  auto loaderOrErr = pefLib->getLoaderInfoHeader();
  if (!loaderOrErr) {
    error(toString(loaderOrErr.takeError()) + " in " + getName());
    return;
  }

  if (config->verbose) {
    errorHandler().outs() << "  Exported symbols: "
                          << loaderOrErr->ExportedSymbolCount << "\n";
  }

  // We don't parse individual exports here - they'll be looked up on demand
  // in findExport() when resolving undefined symbols
}

// Compute PEF export hash for a symbol name
// Algorithm from Mac OS Runtime Architectures (PEFBinaryFormat.h):
//   for each char: hash = PseudoRotate(hash) ^ char
//   where PseudoRotate(x) = ((x << 1) - (x >> 16))
//   result = (length << 16) | ((hash ^ (hash >> 16)) & 0xFFFF)
static uint32_t computePEFHash(StringRef name) {
  int32_t hashValue = 0;

  // Compute hash using PseudoRotate algorithm
  for (char c : name)
    hashValue = ((hashValue << 1) - (hashValue >> 16)) ^ static_cast<uint8_t>(c);

  // Combine with length
  uint16_t finalHash = (hashValue ^ (hashValue >> 16)) & 0xFFFF;
  return (static_cast<uint32_t>(name.size()) << 16) | finalHash;
}

// Find an exported symbol by name in the export hash table
Symbol *SharedLibraryFile::findExport(StringRef name) const {
  using namespace llvm::support;
  using namespace llvm::PEF;

  // Get loader info header
  auto loaderInfoOrErr = pefLib->getLoaderInfoHeader();
  if (!loaderInfoOrErr) {
    if (config->verbose)
      errorHandler().outs() << "  Cannot read loader info from " << getName() << "\n";
    return nullptr;
  }

  const LoaderInfoHeader &loaderInfo = *loaderInfoOrErr;

  // If no exports, return early
  if (loaderInfo.ExportedSymbolCount == 0)
    return nullptr;

  // Find the loader section
  ArrayRef<uint8_t> loaderData;
  bool foundLoader = false;
  for (unsigned i = 0; i < pefLib->getSectionCount(); ++i) {
    auto hdrOrErr = pefLib->getSectionHeader(i);
    if (!hdrOrErr)
      continue;

    if (hdrOrErr->SectionKind == kPEFLoaderSection) {
      auto dataOrErr = pefLib->getSectionData(i);
      if (!dataOrErr) {
        if (config->verbose)
          errorHandler().outs() << "  Cannot read loader section from " << getName() << "\n";
        return nullptr;
      }
      loaderData = *dataOrErr;
      foundLoader = true;
      break;
    }
  }

  if (!foundLoader) {
    if (config->verbose)
      errorHandler().outs() << "  No loader section in " << getName() << "\n";
    return nullptr;
  }

  // Compute hash word for symbol name
  uint32_t fullHashWord = computePEFHash(name);

  // Compute hash table size and slot index
  uint32_t hashTableSize = 1u << loaderInfo.ExportHashTablePower;
  uint32_t slotIndex = fullHashWord % hashTableSize;

  // Calculate offsets for the three parallel arrays
  uint64_t hashSlotTableOffset = loaderInfo.ExportHashOffset;
  uint64_t keyTableOffset = hashSlotTableOffset + hashTableSize * 4;
  uint64_t symbolTableOffset = keyTableOffset + loaderInfo.ExportedSymbolCount * 4;

  // Read the hash slot (4 bytes, big-endian)
  if (hashSlotTableOffset + slotIndex * 4 + 4 > loaderData.size())
    return nullptr;

  const uint8_t *slotPtr = loaderData.data() + hashSlotTableOffset + slotIndex * 4;
  uint32_t slotValue = endian::read32be(slotPtr);

  // Extract chain count and first index
  uint32_t chainCount = getHashSlotChainCount(slotValue);
  uint32_t firstIndex = getHashSlotFirstIndex(slotValue);

  if (chainCount == 0)
    return nullptr; // No exports in this hash slot

  // Scan the chain looking for matching symbol
  for (uint32_t i = 0; i < chainCount; ++i) {
    uint32_t keyIndex = firstIndex + i;

    if (keyIndex >= loaderInfo.ExportedSymbolCount)
      break; // Invalid index

    // Read the hash key (4 bytes, big-endian)
    if (keyTableOffset + keyIndex * 4 + 4 > loaderData.size())
      break;

    const uint8_t *keyPtr = loaderData.data() + keyTableOffset + keyIndex * 4;
    uint32_t keyValue = endian::read32be(keyPtr);

    // Check if hash matches
    if (keyValue != fullHashWord)
      continue; // Hash collision, different symbol

    // Read the exported symbol (10 bytes, big-endian)
    if (symbolTableOffset + keyIndex * 10 + 10 > loaderData.size())
      break;

    const uint8_t *symPtr = loaderData.data() + symbolTableOffset + keyIndex * 10;
    uint32_t classAndName = endian::read32be(symPtr);
    // uint32_t symbolValue = endian::read32be(symPtr + 4);  // Not needed for lookup
    // int16_t sectionIndex = endian::read16be(symPtr + 8);  // Not needed for lookup

    // Extract name offset from classAndName (bits 0-23)
    uint32_t nameOffset = getExportedSymbolNameOffset(classAndName);

    // Read the symbol name from loader string table
    auto nameOrErr = pefLib->getLoaderString(loaderInfo.LoaderStringsOffset + nameOffset);
    if (!nameOrErr)
      continue;

    // Check if names match
    if (*nameOrErr != name)
      continue; // Name mismatch

    // Found matching symbol!
    if (config->verbose) {
      errorHandler().outs() << "  Found export: " << name << " in " << libraryName << "\n";
    }

    // Extract and store symbol class for Driver.cpp to use
    lastSymbolClass = getExportedSymbolClass(classAndName);

    // Return a non-null marker to indicate symbol was found
    // Driver.cpp will call getLastSymbolClass() to get the symbol class
    // then call symtab->addImported() to create the ImportedSymbol
    return reinterpret_cast<Symbol *>(1); // Temporary marker
  }

  return nullptr; // Symbol not found in export table
}

// Create a shared library file from a memory buffer
SharedLibraryFile *createSharedLibraryFile(MemoryBufferRef mb, bool isWeak) {
  // Identify the file type
  file_magic magic = identify_magic(mb.getBuffer());

  // Check if it's a PEF file
  if (magic != file_magic::pef_object) {
    error(mb.getBufferIdentifier() + ": not a PEF file");
    return nullptr;
  }

  // Create and parse the shared library file
  auto *file = make<SharedLibraryFile>(mb, isWeak);
  file->parse();
  return file;
}

} // namespace lld::pef
