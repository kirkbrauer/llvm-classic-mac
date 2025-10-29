//===- OutputSection.cpp --------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "OutputSection.h"
#include "Config.h"
#include "InputFiles.h"
#include "lld/Common/ErrorHandler.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/MathExtras.h"

using namespace llvm;
using namespace lld;
using namespace lld::pef;

void OutputSection::finalizeLayout() {
  if (inputSections.empty()) {
    size = 0;
    return;
  }

  uint64_t offset = 0;

  for (InputSection *isec : inputSections) {
    // Align to the input section's requirement
    uint32_t align = isec->getAlignment();
    offset = alignTo(offset, align);

    // Update alignment requirement
    setAlignment(align);

    // Assign virtual address to input section
    isec->setVirtualAddress(virtualAddress + offset);

    // Add section size
    offset += isec->getSize();

    if (config->verbose) {
      errorHandler().outs() << "    " << isec->getFile()->getName()
                           << ":" << isec->getName()
                           << " offset=0x" << utohexstr(offset - isec->getSize())
                           << " size=0x" << utohexstr(isec->getSize())
                           << " va=0x" << utohexstr(isec->getVirtualAddress())
                           << "\n";
    }
  }

  size = offset;

  if (config->verbose) {
    errorHandler().outs() << "  " << name << " final size: 0x"
                         << utohexstr(size) << " bytes\n";
  }
}
