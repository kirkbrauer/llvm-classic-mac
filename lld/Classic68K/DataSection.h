//===- DataSection.h - Classic 68K data section (DATA 0) ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file generates the DATA 0 resource containing initialized global data
// for classic 68K applications.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_CLASSIC68K_DATASECTION_H
#define LLD_CLASSIC68K_DATASECTION_H

#include "llvm/ADT/ArrayRef.h"
#include <cstdint>
#include <vector>

namespace lld::classic68k {

class DataSection {
public:
  DataSection() = default;

  // Set the initialized data bytes
  void setData(llvm::ArrayRef<uint8_t> data);

  // Set the BSS (zero-initialized) size
  void setBSSSize(uint32_t size) { bssSize = size; }

  // Get the total size needed below A5 (data + bss, where bss includes QD globals)
  uint32_t getBelowA5Size() const;

  // Get the initialized data size
  size_t getDataSize() const { return dataBytes.size(); }

  // Get BSS size
  uint32_t getBSSSize() const { return bssSize; }

  // Generate the DATA 0 resource
  std::vector<uint8_t> generate() const;

  // QuickDraw globals size (fixed at 206 bytes for classic 68K)
  static constexpr uint32_t QD_GLOBALS_SIZE = 206;

private:
  std::vector<uint8_t> dataBytes;
  uint32_t bssSize = 0;
};

} // namespace lld::classic68k

#endif // LLD_CLASSIC68K_DATASECTION_H
