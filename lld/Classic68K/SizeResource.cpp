//===- SizeResource.cpp - Classic 68K SIZE resource -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "SizeResource.h"

namespace lld::classic68k {

std::vector<uint8_t> SizeResource::generate() const {
  std::vector<uint8_t> data;

  // SIZE resource format (10 bytes):
  // +0x00: Flags (2 bytes)
  // +0x02: Preferred size (4 bytes)
  // +0x06: Minimum size (4 bytes)

  // Flags
  data.push_back((flags >> 8) & 0xFF);
  data.push_back(flags & 0xFF);

  // Preferred size
  data.push_back((preferredSize >> 24) & 0xFF);
  data.push_back((preferredSize >> 16) & 0xFF);
  data.push_back((preferredSize >> 8) & 0xFF);
  data.push_back(preferredSize & 0xFF);

  // Minimum size
  data.push_back((minimumSize >> 24) & 0xFF);
  data.push_back((minimumSize >> 16) & 0xFF);
  data.push_back((minimumSize >> 8) & 0xFF);
  data.push_back(minimumSize & 0xFF);

  return data;
}

} // namespace lld::classic68k
