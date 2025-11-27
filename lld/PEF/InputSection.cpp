//===- InputSection.cpp ---------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "InputSection.h"
#include "InputFiles.h"

using namespace llvm;
using namespace lld;
using namespace lld::pef;

ObjFile *InputSection::getPEFFile() const {
  if (fromELF)
    return nullptr;
  return reinterpret_cast<ObjFile *>(inputFile);
}

Expected<ArrayRef<uint8_t>> InputSection::getData() const {
  // For ELF sections, return the stored data
  if (fromELF) {
    return ArrayRef<uint8_t>(elfData);
  }
  // For PEF sections, delegate to the file
  ObjFile *pefFile = getPEFFile();
  if (pefFile)
    return pefFile->getSectionData(sectionIndex);
  return createStringError(std::errc::invalid_argument, "no file available");
}

StringRef InputSection::getName() const {
  // For ELF sections, return the stored name
  if (fromELF) {
    return elfName;
  }
  // For PEF sections, return a generic name based on section kind
  switch (header.SectionKind) {
  case PEF::kPEFCodeSection:
    return ".text";
  case PEF::kPEFUnpackedDataSection:
  case PEF::kPEFPatternDataSection:
    return ".data";
  case PEF::kPEFConstantSection:
    return ".rodata";
  case PEF::kPEFLoaderSection:
    return ".loader";
  default:
    return ".unknown";
  }
}
