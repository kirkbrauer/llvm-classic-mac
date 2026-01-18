//===- DataSection.cpp - Classic 68K data section (DATA 0) ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "DataSection.h"

namespace lld::classic68k {

void DataSection::setData(llvm::ArrayRef<uint8_t> data) {
  dataBytes.assign(data.begin(), data.end());
}

uint32_t DataSection::getBelowA5Size() const {
  // Below A5 = initialized data + BSS + runtime padding
  // Note: BSS already includes QD globals from macos_classic_qd.o (206 bytes)
  //
  // Memory layout calculation:
  //   - dataBytes: initialized data (rodata strings, etc.)
  //   - bssSize: uninitialized data (QD globals = 206 bytes)
  //   - extra padding: for alignment and runtime state (38 bytes)
  //
  // Symbol offset calculation uses: a5Offset = -(belowA5Size - symbolAddress)
  // For qd at address 38 (after 38 bytes of rodata):
  //   a5Offset = -(282 - 38) = -244
  //   qd.thePort = -244 + 202 = -42 (matches CodeWarrior)
  //
  // Note: CodeWarrior uses 288 bytes, but with a different layout.
  // Our 282 bytes works because our symbol addresses are consistent.
  return dataBytes.size() + bssSize + 38;
}

std::vector<uint8_t> DataSection::generate() const {
  // DATA 0 resource contains the initialized data that will be copied
  // to the application's A5 world at launch time.
  //
  // Format:
  //   +0x00: Size of data to copy (4 bytes, big-endian)
  //   +0x04: Initialized data bytes
  //
  // BSS is not stored - it's implicitly zeroed by the OS.
  // The startup code reads the size, then copies that many bytes
  // starting from offset +4 to the below-A5 area.

  std::vector<uint8_t> data;

  // First 4 bytes: size of data to copy (big-endian)
  uint32_t copySize = dataBytes.size();
  data.push_back((copySize >> 24) & 0xFF);
  data.push_back((copySize >> 16) & 0xFF);
  data.push_back((copySize >> 8) & 0xFF);
  data.push_back(copySize & 0xFF);

  // Then the actual data bytes
  data.insert(data.end(), dataBytes.begin(), dataBytes.end());
  return data;
}

} // namespace lld::classic68k
