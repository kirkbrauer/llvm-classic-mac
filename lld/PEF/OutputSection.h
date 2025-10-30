//===- OutputSection.h ------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_PEF_OUTPUT_SECTION_H
#define LLD_PEF_OUTPUT_SECTION_H

#include "InputSection.h"
#include "lld/Common/LLVM.h"
#include "llvm/BinaryFormat/PEF.h"
#include <vector>

namespace lld::pef {

// Represents an output section that contains merged input sections
class OutputSection {
public:
  OutputSection(StringRef name, uint8_t kind)
      : name(name), sectionKind(kind) {}

  // Get section name
  StringRef getName() const { return name; }

  // Get section kind
  uint8_t getKind() const { return sectionKind; }

  // Add an input section
  void addInputSection(InputSection *isec) {
    inputSections.push_back(isec);
  }

  // Get all input sections
  ArrayRef<InputSection *> getInputSections() const { return inputSections; }

  // Get section size (sum of all input sections, aligned)
  uint64_t getSize() const { return size; }
  void setSize(uint64_t s) { size = s; }

  // Virtual address assigned during layout
  uint64_t getVirtualAddress() const { return virtualAddress; }
  void setVirtualAddress(uint64_t addr) { virtualAddress = addr; }

  // File offset in output file
  uint64_t getFileOffset() const { return fileOffset; }
  void setFileOffset(uint64_t offset) { fileOffset = offset; }

  // Alignment requirement (maximum of all input sections)
  uint32_t getAlignment() const { return alignment; }
  void setAlignment(uint32_t align) {
    if (align > alignment)
      alignment = align;
  }

  // Compute final size by laying out input sections
  void finalizeLayout();

private:
  StringRef name;
  uint8_t sectionKind;
  std::vector<InputSection *> inputSections;
  uint64_t size = 0;
  uint64_t virtualAddress = 0;
  uint64_t fileOffset = 0;
  uint32_t alignment = 16;  // CodeWarrior uses 16-byte alignment
};

} // namespace lld::pef

#endif
