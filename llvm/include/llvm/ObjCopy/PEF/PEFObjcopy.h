//===- PEFObjcopy.h ---------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_OBJCOPY_PEF_PEFOBJCOPY_H
#define LLVM_OBJCOPY_PEF_PEFOBJCOPY_H

namespace llvm {
class Error;
class raw_ostream;

namespace object {
class PEFObjectFile;
} // namespace object

namespace objcopy {
struct CommonConfig;
struct PEFConfig;

namespace pef {

/// Execute objcopy operation on a PEF object file
Error executeObjcopyOnBinary(const CommonConfig &Config,
                             const PEFConfig &PEFConfig,
                             object::PEFObjectFile &In,
                             raw_ostream &Out);

} // namespace pef
} // namespace objcopy
} // namespace llvm

#endif // LLVM_OBJCOPY_PEF_PEFOBJCOPY_H
