//===--- MacOSClassic.h - Classic Mac OS ToolChain -------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the Classic Mac OS (System 7-9) ToolChain.
// Classic Mac OS is NOT a Darwin-based system and predates Mac OS X.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_MACOSCLASSIC_H
#define LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_MACOSCLASSIC_H

#include "clang/Driver/Driver.h"
#include "clang/Driver/ToolChain.h"

namespace clang {
namespace driver {
namespace toolchains {

/// MacOSClassic - Classic Mac OS (System 7, 8, and 9) toolchain.
/// This is separate from Darwin toolchain as Classic Mac OS predates
/// the Darwin-based Mac OS X and has a completely different architecture.
class LLVM_LIBRARY_VISIBILITY MacOSClassic : public ToolChain {
public:
  MacOSClassic(const Driver &D, const llvm::Triple &Triple,
               const llvm::opt::ArgList &Args);

  bool isPICDefault() const override { return false; }
  bool isPIEDefault(const llvm::opt::ArgList &Args) const override {
    return false;
  }
  bool isPICDefaultForced() const override { return false; }

  bool HasNativeLLVMSupport() const override { return true; }

  // Classic Mac OS does not use modern C++ ABI
  bool IsObjCNonFragileABIDefault() const override { return false; }

  // Classic Mac OS toolchain properties
  bool IsMathErrnoDefault() const override { return false; }

  unsigned GetDefaultDwarfVersion() const override { return 2; }

protected:
  Tool *buildLinker() const override;
  Tool *buildAssembler() const override;
};

} // end namespace toolchains
} // end namespace driver
} // end namespace clang

#endif // LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_MACOSCLASSIC_H
