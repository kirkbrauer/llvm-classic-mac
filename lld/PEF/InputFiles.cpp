//===- InputFiles.cpp -----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "InputFiles.h"
#include "Config.h"
#include "ELFInputFiles.h"
#include "InputSection.h"
#include "SymbolTable.h"
#include "lld/Common/ErrorHandler.h"
#include "lld/Common/Memory.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/Magic.h"
#include "llvm/BinaryFormat/PEF.h"
#include "llvm/Object/Archive.h"
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
    // Store original value for sparse TVector layout (compiler hard-codes offsets)
    definedSym->setOriginalValue(value);
    symbols.push_back(definedSym);
  }

  // Phase 2 - Scan all imports from loader section
  // Build importIndexMap BEFORE processing relocations to avoid "invalid import index" errors
  auto loaderInfoOrErr = pefObj->getLoaderInfoHeader();
  if (loaderInfoOrErr) {
    const PEF::LoaderInfoHeader &loaderInfo = *loaderInfoOrErr;

    // Scan ALL imported symbols and add to importIndexMap
    if (loaderInfo.TotalImportedSymbolCount > 0) {
      if (config->verbose) {
        errorHandler().outs() << "  Scanning " << loaderInfo.TotalImportedSymbolCount
                             << " imported symbols\n";
      }

      for (uint32_t i = 0; i < loaderInfo.TotalImportedSymbolCount; ++i) {
        auto symNameOrErr = pefObj->getImportedSymbolName(i);
        if (symNameOrErr) {
          StringRef symName = *symNameOrErr;

          // Check if symbol is already defined
          Symbol *existing = symtab->find(symName);
          if (existing && existing->isDefined()) {
            // Symbol is defined internally - store it but mark it as resolved
            importIndexMap[i] = existing;
            if (config->verbose) {
              Defined *def = cast<Defined>(existing);
              errorHandler().outs() << "  Resolved undefined symbol '" << symName
                                   << "' to defined at section " << def->getSectionIndex()
                                   << " offset 0x" << utohexstr(def->getValue()) << "\n";
              errorHandler().outs() << "    DEBUG: Added to importIndexMap[" << i << "] = "
                                   << (void*)existing << " (file: " << getName()
                                   << " this=" << (void*)this << ")\n";
            }
          } else {
            // Truly undefined symbol - add to symbol table
            Symbol *sym = symtab->addUndefined(symName, this);
            importIndexMap[i] = sym;

            if (config->verbose) {
              errorHandler().outs() << "  Undefined symbol: " << symName << "\n";
              errorHandler().outs() << "    Import " << i << ": " << symName << "\n";
            }
          }
        }
      }
    }
  }

  // Phase 3 - Read relocations from loader section
  loaderInfoOrErr = pefObj->getLoaderInfoHeader();
  if (loaderInfoOrErr) {
    const PEF::LoaderInfoHeader &loaderInfo = *loaderInfoOrErr;

    if (config->verbose && loaderInfo.RelocSectionCount > 0) {
      errorHandler().outs() << "  Reading " << loaderInfo.RelocSectionCount
                           << " relocation sections\n";
    }

    // Read relocation headers (one per section with relocations)
    // Headers are stored in loader section layout:
    // LoaderInfoHeader(56) + ImportedLibs + ImportedSyms + RelocHeaders + RelocInstrs
    // Note: RelocInstrOffset points to instructions, NOT headers!
    for (unsigned i = 0; i < loaderInfo.RelocSectionCount; ++i) {
      uint64_t headerOffset = 56 +  // After LoaderInfoHeader
                              loaderInfo.ImportedLibraryCount * 28 +  // ImportedLibrary entries
                              loaderInfo.TotalImportedSymbolCount * 4 +  // ImportedSymbol entries
                              i * 12;  // LoaderRelocationHeader entries (12 bytes each)
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
          // PEF relocation instructions: [opcode:7][operand:9] per Apple spec
          uint8_t opcode = (instr >> 9) & 0x7F;

          switch (opcode) {
            case PEF::kPEFRelocSmByImport: {
              // Small import reference (index in operand)
              // NOTE: Import scanning in Phase 2 already populated importIndexMap,
              // so we skip this to avoid overwriting resolved symbols
              break;
            }

            case PEF::kPEFRelocLgByImport: {
              // Large import reference (2 instructions)
              // NOTE: Import scanning in Phase 2 already populated importIndexMap,
              // so we just skip the second instruction and continue
              if (j + 1 < relocs.size()) {
                j++; // Skip second instruction
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

  // Check for ELF object file (primary format)
  if (magic == file_magic::elf_relocatable) {
    return createELFObjectFile(mb, archiveName);
  }

  // Check for XCOFF object file (MPW static libraries like OpenTransportAppPPC.o)
  if (magic == file_magic::xcoff_object_32) {
    return createXCOFFObjectFile(mb, archiveName);
  }

  // Check for PEF object file (legacy support - deprecated)
  if (magic == file_magic::pef_object) {
    if (config->verbose) {
      errorHandler().outs()
          << "Warning: PEF object files are deprecated. "
          << "Use ELF object files instead.\n";
    }
    auto *file = make<ObjFile>(mb, archiveName);
    file->parse();
    return file;
  }

  error(mb.getBufferIdentifier() + ": unknown file type (expected ELF or XCOFF object)");
  return nullptr;
}

// Process an archive (.rlib, .a) and return all object files within
std::vector<InputFile *> createObjectFilesFromArchive(MemoryBufferRef mb) {
  std::vector<InputFile *> files;
  StringRef archivePath = mb.getBufferIdentifier();

  // Open the archive
  auto archiveOrErr = object::Archive::create(mb);
  if (!archiveOrErr) {
    error(toString(archiveOrErr.takeError()) + " in " + archivePath);
    return files;
  }

  object::Archive &archive = *archiveOrErr.get();

  if (config->verbose) {
    errorHandler().outs() << "Processing archive: " << archivePath << "\n";
  }

  // Iterate over all children (members) in the archive
  Error err = Error::success();
  for (const object::Archive::Child &child : archive.children(err)) {
    // Get the child's buffer
    auto bufOrErr = child.getBuffer();
    if (!bufOrErr) {
      error(toString(bufOrErr.takeError()) + " in " + archivePath);
      continue;
    }

    // Get the child's name
    auto nameOrErr = child.getName();
    StringRef childName = nameOrErr ? *nameOrErr : "<unknown>";

    // Skip non-object files (like .rmeta metadata files in rlibs)
    if (childName.ends_with(".rmeta") || childName.ends_with(".rlib")) {
      if (config->verbose) {
        errorHandler().outs() << "  Skipping metadata: " << childName << "\n";
      }
      continue;
    }

    // Create a MemoryBufferRef for the child
    MemoryBufferRef childMb(*bufOrErr, childName);

    // Check if it's an object file we can handle
    file_magic childMagic = identify_magic(childMb.getBuffer());
    if (childMagic == file_magic::elf_relocatable) {
      if (config->verbose) {
        errorHandler().outs() << "  Loading ELF object: " << childName << "\n";
      }
      if (InputFile *file = createELFObjectFile(childMb, archivePath.str())) {
        files.push_back(file);
      }
    } else if (childMagic == file_magic::xcoff_object_32) {
      if (config->verbose) {
        errorHandler().outs() << "  Loading XCOFF object: " << childName << "\n";
      }
      if (InputFile *file = createXCOFFObjectFile(childMb, archivePath.str())) {
        files.push_back(file);
      }
    } else if (childMagic == file_magic::pef_object) {
      if (config->verbose) {
        errorHandler().outs() << "  Loading PEF object: " << childName << "\n";
      }
      auto *file = make<ObjFile>(childMb, archivePath.str());
      file->parse();
      files.push_back(file);
    } else {
      // Skip unknown file types silently (could be metadata, etc.)
      if (config->verbose) {
        errorHandler().outs() << "  Skipping unknown type: " << childName << "\n";
      }
    }
  }

  if (err) {
    error(toString(std::move(err)) + " in " + archivePath);
  }

  if (config->verbose) {
    errorHandler().outs() << "  Loaded " << files.size() << " object(s) from archive\n";
  }

  return files;
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
// Handles concatenated PEF containers (e.g., OpenTransportLib has 9 containers)
void SharedLibraryFile::parse() {
  if (config->verbose) {
    errorHandler().outs() << "Parsing PEF shared library: " << getName()
                          << " (" << libraryName << ")\n";
  }

  // Scan for all PEF containers in the file (look for 'Joy!' magic)
  StringRef data = mb.getBuffer();
  const uint8_t *ptr = reinterpret_cast<const uint8_t *>(data.data());
  size_t remaining = data.size();
  size_t offset = 0;
  unsigned containerIndex = 0;

  while (remaining >= sizeof(PEF::ContainerHeader)) {
    // Check for PEF magic: 'Joy!' (0x4A6F7921) followed by 'peff' (0x70656666)
    uint32_t tag1 = support::endian::read32be(ptr);
    uint32_t tag2 = support::endian::read32be(ptr + 4);

    if (tag1 != PEF::kPEFTag1 || tag2 != PEF::kPEFTag2) {
      // Not a PEF header at this position, move forward and scan
      // This shouldn't happen at the start but might between containers
      ptr++;
      offset++;
      remaining--;
      continue;
    }

    // Found a PEF container header
    // Create a sub-buffer for this container
    // We need to determine the container size from the header

    // Read container header to get section count
    PEF::ContainerHeader hdr = object::PEFSupport::readContainerHeader(ptr);

    // Calculate container size:
    // Header (40 bytes) + section headers (28 bytes each) + section data
    size_t headerSize = sizeof(PEF::ContainerHeader);
    size_t sectionHeadersSize = hdr.SectionCount * PEF::kSectionHeaderFileSize;

    // We need to read all section headers to find the maximum extent
    size_t containerSize = headerSize + sectionHeadersSize;
    const uint8_t *secHdrPtr = ptr + headerSize;

    for (uint16_t i = 0; i < hdr.SectionCount; ++i) {
      if (secHdrPtr + PEF::kSectionHeaderFileSize > ptr + remaining)
        break;

      PEF::SectionHeader secHdr = object::PEFSupport::readSectionHeader(secHdrPtr);
      size_t sectionEnd = secHdr.ContainerOffset + secHdr.ContainerLength;
      if (sectionEnd > containerSize)
        containerSize = sectionEnd;

      secHdrPtr += PEF::kSectionHeaderFileSize;
    }

    // Ensure we don't exceed the file
    if (containerSize > remaining)
      containerSize = remaining;

    // Create a memory buffer for this container
    StringRef containerData(reinterpret_cast<const char *>(ptr), containerSize);
    MemoryBufferRef containerMB(containerData, getName());

    // Parse this container
    auto objOrErr = PEFObjectFile::create(containerMB);
    if (!objOrErr) {
      // Log error but continue to next container
      if (config->verbose) {
        errorHandler().outs() << "  Warning: Failed to parse container "
                             << containerIndex << " at offset "
                             << offset << ": "
                             << toString(objOrErr.takeError()) << "\n";
      } else {
        consumeError(objOrErr.takeError());
      }
    } else {
      auto &pef = *objOrErr;

      // Get export count for logging
      unsigned exportCount = 0;
      auto loaderOrErr = pef->getLoaderInfoHeader();
      if (loaderOrErr) {
        exportCount = loaderOrErr->ExportedSymbolCount;
      }

      if (config->verbose) {
        errorHandler().outs() << "  Container " << containerIndex
                             << " at offset " << offset
                             << ": " << hdr.SectionCount << " sections, "
                             << exportCount << " exports\n";
      }

      pefContainers.push_back(std::move(pef));
    }

    // Move to next potential container
    ptr += containerSize;
    offset += containerSize;
    remaining -= containerSize;
    containerIndex++;

    // Align to next 4-byte boundary (PEF containers are typically aligned)
    while (remaining > 0 && (offset & 3) != 0) {
      ptr++;
      offset++;
      remaining--;
    }
  }

  if (config->verbose) {
    errorHandler().outs() << "  Total containers: " << pefContainers.size() << "\n";
  }

  if (pefContainers.empty()) {
    error("no valid PEF containers found in " + getName());
  }
}

// Compute PEF export hash for a symbol name
// Algorithm from Mac OS Runtime Architectures (PEFBinaryFormat.h):
//   for each char: hash = PseudoRotate(hash) ^ char
//   where PseudoRotate(x) = ((x << 1) - (x >> 16))
//   result = (length << 16) | ((hash ^ (hash >> 16)) & 0xFFFF)
static uint32_t computePEFHash(StringRef name) {
  int32_t hashValue = 0;

  // Compute hash using PseudoRotate algorithm
  // IMPORTANT: Do NOT cast 'c' to uint8_t! The char type must remain signed
  // so that characters with bit 7 set (0x80-0xFF) are sign-extended before XOR.
  // This matches Apple's PEFBinaryFormat.h specification and Retro68 implementation.
  for (char c : name)
    hashValue = ((hashValue << 1) - (hashValue >> 16)) ^ c;

  // Combine with length
  uint16_t finalHash = (hashValue ^ (hashValue >> 16)) & 0xFFFF;
  return (static_cast<uint32_t>(name.size()) << 16) | finalHash;
}

// Find an exported symbol by name (searches all containers)
Symbol *SharedLibraryFile::findExport(StringRef name) const {
  // Search through all PEF containers
  for (const auto &pef : pefContainers) {
    if (Symbol *sym = findExportInContainer(pef.get(), name))
      return sym;
  }
  return nullptr;
}

// Find an exported symbol by name in a specific container's export hash table
Symbol *SharedLibraryFile::findExportInContainer(PEFObjectFile *pefLib,
                                                   StringRef name) const {
  using namespace llvm::support;
  using namespace llvm::PEF;

  // Get loader info header
  auto loaderInfoOrErr = pefLib->getLoaderInfoHeader();
  if (!loaderInfoOrErr) {
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
        return nullptr;
      }
      loaderData = *dataOrErr;
      foundLoader = true;
      break;
    }
  }

  if (!foundLoader) {
    return nullptr;
  }

  // Compute hash word for symbol name
  uint32_t fullHashWord = computePEFHash(name);

  // Compute hash table size and slot index
  uint32_t hashTableSize = 1u << loaderInfo.ExportHashTablePower;
  // Use XOR folding per Apple's PEFHashTableIndex macro, not modulo:
  //   PEFHashTableIndex(fullHashWord, hashTablePower) =
  //     ((fullHashWord) ^ ((fullHashWord) >> (hashTablePower))) & ((1 << (hashTablePower)) - 1)
  // This provides better hash distribution than simple modulo.
  uint32_t slotIndex = (fullHashWord ^ (fullHashWord >> loaderInfo.ExportHashTablePower))
                       & (hashTableSize - 1);

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

    // Extract name offset from classAndName (bits 0-23)
    uint32_t nameOffset = getExportedSymbolNameOffset(classAndName);

    // Read the symbol name from loader string table
    // IMPORTANT: MPW stub libraries use concatenated strings without null terminators!
    // We need to scan the string table to find ALL name offsets, then determine length.

    // For now, use a simpler heuristic: read up to 256 chars and look for the search name
    uint64_t stringOffset = loaderInfo.LoaderStringsOffset + nameOffset;
    if (stringOffset >= loaderData.size()) {
      continue;
    }

    const char *strStart = reinterpret_cast<const char *>(loaderData.data() + stringOffset);
    uint32_t maxLen = std::min(256u, static_cast<uint32_t>(loaderData.size() - stringOffset));

    // Find the actual symbol name by scanning for the pattern we're looking for
    // Strategy: Read the concatenated blob and check if it STARTS WITH our target name
    StringRef blob(strStart, maxLen);

    // Check if the blob starts with our target name
    if (!blob.starts_with(name))
      continue; // Name mismatch

    // SUCCESS! The symbol name at this offset starts with our target
    // (this works because symbol names are unique prefixes in the concatenated string)

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

  return nullptr; // Symbol not found in this container
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
