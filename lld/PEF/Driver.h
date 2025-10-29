//===- Driver.h -------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_PEF_DRIVER_H
#define LLD_PEF_DRIVER_H

#include "lld/Common/LLVM.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Option/OptTable.h"

namespace lld {
class CommonLinkerContext;
}

namespace lld::pef {

class PEFOptTable : public llvm::opt::GenericOptTable {
public:
  PEFOptTable();
  llvm::opt::InputArgList parse(CommonLinkerContext &ctx,
                                ArrayRef<const char *> argv);
};

// Main linker entry point
bool link(llvm::ArrayRef<const char *> args, llvm::raw_ostream &stdoutOS,
          llvm::raw_ostream &stderrOS, bool exitEarly, bool disableOutput);

} // namespace lld::pef

#endif
