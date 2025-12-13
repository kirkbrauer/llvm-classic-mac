//===- MarkLive.h ------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares the garbage collection interface for the PEF linker.
// The markLive function implements --gc-sections by marking reachable sections
// starting from GC roots (entry point, exported symbols).
//
//===----------------------------------------------------------------------===//

#ifndef LLD_PEF_MARKLIVE_H
#define LLD_PEF_MARKLIVE_H

#include <vector>

namespace lld::pef {

class OutputSection;
class SymbolTable;

/// Mark live sections starting from GC roots.
/// This implements --gc-sections garbage collection.
void markLive(std::vector<OutputSection *> &outputSections,
              SymbolTable *symtab);

} // namespace lld::pef

#endif // LLD_PEF_MARKLIVE_H
