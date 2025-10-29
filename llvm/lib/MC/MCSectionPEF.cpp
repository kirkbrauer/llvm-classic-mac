//===- lib/MC/MCSectionPEF.cpp - PEF Code Section ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the MCSectionPEF class.
//
//===----------------------------------------------------------------------===//

#include "llvm/MC/MCSectionPEF.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

void MCSectionPEF::printSwitchToSection(const MCAsmInfo &MAI, const Triple &,
                                        raw_ostream &OS,
                                        uint32_t Subsection) const {
  // Print the section directive
  // For PEF, we support .text, .data, .bss, .rodata sections
  OS << '\t' << getName() << '\n';
}
