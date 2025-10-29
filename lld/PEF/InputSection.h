//===- InputSection.h -------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_PEF_INPUT_SECTION_H
#define LLD_PEF_INPUT_SECTION_H

#include "lld/Common/LLVM.h"
#include "llvm/BinaryFormat/PEF.h"
#include "llvm/Support/Error.h"
#include <cstdint>

namespace lld::pef {

class ObjFile;

// Represents a section from an input PEF object file
class InputSection {
public:
  InputSection(ObjFile *file, unsigned index, const llvm::PEF::SectionHeader &hdr)
      : file(file), sectionIndex(index), header(hdr) {}

  // Get the owning file
  ObjFile *getFile() const { return file; }

  // Get the section index in the input file
  unsigned getIndex() const { return sectionIndex; }

  // Get the section header
  const llvm::PEF::SectionHeader &getHeader() const { return header; }

  // Get section kind (code, data, etc.)
  uint8_t getKind() const { return header.SectionKind; }

  // Get section size in memory
  uint64_t getSize() const { return header.TotalLength; }

  // Get unpacked data size
  uint64_t getUnpackedSize() const { return header.UnpackedLength; }

  // Get section data from the input file
  Expected<ArrayRef<uint8_t>> getData() const;

  // Get section name (if any)
  StringRef getName() const;

  // Virtual address assigned during layout (0 if not assigned yet)
  uint64_t getVirtualAddress() const { return virtualAddress; }
  void setVirtualAddress(uint64_t addr) { virtualAddress = addr; }

  // Alignment requirement (power of 2)
  uint32_t getAlignment() const { return 1U << header.Alignment; }

private:
  ObjFile *file;
  unsigned sectionIndex;
  llvm::PEF::SectionHeader header;
  uint64_t virtualAddress = 0;
};

} // namespace lld::pef

#endif
