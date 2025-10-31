//===-- PEFDump.h - PEF-specific dumper -------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares PEF-specific functions for llvm-objdump.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_OBJDUMP_PEFDUMP_H
#define LLVM_TOOLS_LLVM_OBJDUMP_PEFDUMP_H

#include "llvm/ADT/SmallVector.h"
#include <memory>

namespace llvm {

class Error;

namespace object {
class PEFObjectFile;
class RelocationRef;
} // namespace object

namespace objdump {

class Dumper;

std::unique_ptr<Dumper> createPEFDumper(const object::PEFObjectFile &Obj);

Error getPEFRelocationValueString(const object::PEFObjectFile *Obj,
                                  const object::RelocationRef &RelRef,
                                  llvm::SmallVectorImpl<char> &Result);

void printPEFFileHeader(const object::PEFObjectFile *Obj);

} // namespace objdump
} // namespace llvm

#endif // LLVM_TOOLS_LLVM_OBJDUMP_PEFDUMP_H
