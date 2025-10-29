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

Expected<ArrayRef<uint8_t>> InputSection::getData() const {
  return file->getSectionData(sectionIndex);
}

StringRef InputSection::getName() const {
  // For now, return a generic name based on section kind
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
