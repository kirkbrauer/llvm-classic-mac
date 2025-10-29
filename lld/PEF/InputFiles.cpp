//===- InputFiles.cpp -----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "InputFiles.h"
#include "Config.h"
#include "SymbolTable.h"
#include "lld/Common/ErrorHandler.h"
#include "lld/Common/Memory.h"
#include "llvm/BinaryFormat/Magic.h"
#include "llvm/Object/PEFObjectFile.h"
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

  // Phase 1.3 - Extract symbols
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

  if (config->verbose) {
    errorHandler().outs() << "  Symbols: " << symbols.size() << "\n";
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

} // namespace lld::pef
