//===- CodeSegment.cpp - Classic 68K code segment (CODE n) ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "CodeSegment.h"

namespace lld::classic68k {

void CodeSegment::setCode(llvm::ArrayRef<uint8_t> code) {
  codeBytes.assign(code.begin(), code.end());
}

void CodeSegment::addFunction(const std::string &name, uint32_t offset,
                               uint32_t size, bool isEntry, uint16_t jtIndex) {
  FunctionInfo info;
  info.name = name;
  info.offset = offset;
  info.size = size;
  info.isEntry = isEntry;
  info.jtIndex = jtIndex;
  functions.push_back(std::move(info));
}

std::vector<uint8_t> CodeSegment::generate(uint16_t firstJTOffset,
                                            uint16_t numJTEntries) const {
  std::vector<uint8_t> data;

  // Near model segment header (4 bytes):
  // +0x00: First jump table entry offset from JT start (2 bytes)
  // +0x02: Number of JT entries for this segment (2 bytes)

  // First JT entry offset (offset in bytes from start of jump table)
  data.push_back((firstJTOffset >> 8) & 0xFF);
  data.push_back(firstJTOffset & 0xFF);

  // Number of JT entries
  data.push_back((numJTEntries >> 8) & 0xFF);
  data.push_back(numJTEntries & 0xFF);

  // Append the code bytes
  data.insert(data.end(), codeBytes.begin(), codeBytes.end());

  return data;
}

} // namespace lld::classic68k
