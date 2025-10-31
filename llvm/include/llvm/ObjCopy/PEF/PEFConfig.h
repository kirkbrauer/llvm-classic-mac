//===- PEFConfig.h ----------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_OBJCOPY_PEF_PEFCONFIG_H
#define LLVM_OBJCOPY_PEF_PEFCONFIG_H

namespace llvm {
namespace objcopy {

// PEF-specific configuration options
struct PEFConfig {
  // Currently no PEF-specific options
  // This can be extended in the future for PEF-specific transformations
};

} // namespace objcopy
} // namespace llvm

#endif // LLVM_OBJCOPY_PEF_PEFCONFIG_H
