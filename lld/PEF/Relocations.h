//===- Relocations.h --------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_PEF_RELOCATIONS_H
#define LLD_PEF_RELOCATIONS_H

#include "lld/Common/LLVM.h"
#include <cstdint>

namespace lld::pef {

class InputSection;
class Symbol;

// Process relocations for a given input section
// For Phase 1: Basic validation and preparation
// For Phase 2: Full PEF relocation processing (imports, etc.)
void processRelocations(InputSection *isec);

// Scan all relocations to determine which symbols are needed
// This is important for lazy symbol resolution in Phase 2
void scanRelocations(InputSection *isec);

} // namespace lld::pef

#endif
