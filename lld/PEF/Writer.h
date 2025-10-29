//===- Writer.h -------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_PEF_WRITER_H
#define LLD_PEF_WRITER_H

#include <vector>

namespace lld::pef {

class OutputSection;

// Write the final PEF executable to disk
void writeResult(std::vector<OutputSection *> outputSections);

} // namespace lld::pef

#endif
