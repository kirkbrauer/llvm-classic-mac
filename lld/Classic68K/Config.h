//===- Config.h - Classic 68K linker configuration --------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_CLASSIC68K_CONFIG_H
#define LLD_CLASSIC68K_CONFIG_H

#include "llvm/ADT/StringRef.h"
#include <string>
#include <vector>

namespace lld::classic68k {

struct Config {
  std::string outputFile;
  std::string entrySymbol = "main";
  std::string segmentName = "Sources";
  std::string creatorCode = "????";

  std::vector<std::string> searchPaths;
  std::vector<std::string> inputFiles;

  // Memory sizes
  uint32_t stackSize = 32768;        // 32KB default stack
  uint32_t heapSize = 393216;        // 384KB default heap
  uint32_t minHeapSize = 393216;     // 384KB minimum heap

  // SIZE resource flags
  // Default: acceptSuspendResumeEvents | doesActivateOnFGSwitch |
  //          is32BitCompatible | highLevelEventAware
  uint16_t sizeFlags = 0x58A0;

  bool verbose = false;
  bool relocatable = false;  // -r flag (not supported)
};

extern Config *config;

} // namespace lld::classic68k

#endif // LLD_CLASSIC68K_CONFIG_H
