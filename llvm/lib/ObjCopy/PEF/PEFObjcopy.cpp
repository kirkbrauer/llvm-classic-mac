//===- PEFObjcopy.cpp -----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ObjCopy/PEF/PEFObjcopy.h"
#include "llvm/ObjCopy/CommonConfig.h"
#include "llvm/ObjCopy/PEF/PEFConfig.h"
#include "llvm/Object/PEFObjectFile.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/Error.h"

namespace llvm {
namespace objcopy {
namespace pef {

Error executeObjcopyOnBinary(const CommonConfig &Config,
                             const PEFConfig &PEFConfig,
                             object::PEFObjectFile &In,
                             raw_ostream &Out) {
  // For now, implement a minimal pass-through that just copies the file
  // unchanged. This provides basic functionality and can be enhanced later
  // to support actual transformations.

  StringRef Data = In.getData();
  Out.write(Data.data(), Data.size());

  return Error::success();
}

} // namespace pef
} // namespace objcopy
} // namespace llvm
