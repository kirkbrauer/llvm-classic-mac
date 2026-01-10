//===- Config.h -------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_PEF_CONFIG_H
#define LLD_PEF_CONFIG_H

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include <vector>

namespace lld::pef {

/// Target architecture for PEF output
enum class PEFArch {
  Unknown,  // Not yet determined (initial state)
  PowerPC,  // 'pwpc' - PowerPC architecture
  M68k,     // 'm68k' - Motorola 68K architecture (CFM-68K)
};

struct Config {
  llvm::StringRef entry;          // Entry point symbol name
  llvm::StringRef outputFile;     // Output PEF file path
  std::vector<llvm::StringRef> inputFiles;  // Input object files

  // Base addresses for sections
  uint64_t baseCode = 0x0;        // Code section base address
  uint64_t baseData = 0x0;        // Data section base address

  // Library search paths (Phase 2)
  std::vector<std::string> libraryPaths;

  // PEF shared libraries to link (Phase 2)
  std::vector<std::string> libraries;      // -l
  std::vector<std::string> weakLibraries;  // --weak-l

  // Linker behavior
  bool verbose = false;
  bool allowUndefined = false;
  bool exportDynamic = false;  // Export symbols from executables

  // Garbage collection
  bool gcSections = false;       // --gc-sections
  bool printGcSections = false;  // --print-gc-sections

  // Target architecture (detected from input files)
  PEFArch architecture = PEFArch::Unknown;
};

// The global configuration
extern Config *config;

} // namespace lld::pef

#endif
