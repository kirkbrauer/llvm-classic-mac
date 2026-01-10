//===- Writer.h - Classic 68K application writer -----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file coordinates the generation of classic 68K applications from
// M68k ELF object files.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_CLASSIC68K_WRITER_H
#define LLD_CLASSIC68K_WRITER_H

#include "llvm/ADT/StringRef.h"
#include <string>
#include <vector>

namespace lld::classic68k {

class ELFReader;

class Writer {
public:
  Writer() = default;

  // Add an input file
  void addInput(ELFReader *reader) { inputs.push_back(reader); }

  // Set output file path
  void setOutput(llvm::StringRef path) { outputPath = path.str(); }

  // Link and write the output
  bool link();

  // Get error message
  llvm::StringRef getError() const { return errorMsg; }

private:
  std::vector<ELFReader *> inputs;
  std::string outputPath;
  std::string errorMsg;
};

} // namespace lld::classic68k

#endif // LLD_CLASSIC68K_WRITER_H
