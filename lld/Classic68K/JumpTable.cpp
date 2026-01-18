//===- JumpTable.cpp - Classic 68K jump table (CODE 0) --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "JumpTable.h"

namespace lld::classic68k {

void JumpTable::addEntry(const std::string &name, uint16_t segment,
                          uint16_t offset) {
  JumpTableEntry entry;
  entry.name = name;
  entry.segmentNum = segment;
  entry.offsetInSegment = offset;
  entries.push_back(std::move(entry));
}

uint32_t JumpTable::getAboveA5Size() const {
  // Above A5 = application parameters (32 bytes) + jump table + reserved (8 bytes)
  // The extra 8 bytes match CodeWarrior's layout for segment loader state
  return JT_OFFSET + entries.size() * ENTRY_SIZE + 8;
}

std::vector<uint8_t> JumpTable::generate() const {
  std::vector<uint8_t> data;

  // CODE 0 header (16 bytes):
  // +0x00: Above A5 size (4 bytes) - JT_OFFSET + JT size
  // +0x04: Below A5 size (4 bytes) - globals + QD globals
  // +0x08: Jump table size (4 bytes)
  // +0x0C: Jump table offset from A5 (4 bytes) - always 32

  uint32_t above = getAboveA5Size();
  uint32_t jtSize = entries.size() * ENTRY_SIZE;

  // Above A5 size
  data.push_back((above >> 24) & 0xFF);
  data.push_back((above >> 16) & 0xFF);
  data.push_back((above >> 8) & 0xFF);
  data.push_back(above & 0xFF);

  // Below A5 size
  data.push_back((belowA5 >> 24) & 0xFF);
  data.push_back((belowA5 >> 16) & 0xFF);
  data.push_back((belowA5 >> 8) & 0xFF);
  data.push_back(belowA5 & 0xFF);

  // Jump table size
  data.push_back((jtSize >> 24) & 0xFF);
  data.push_back((jtSize >> 16) & 0xFF);
  data.push_back((jtSize >> 8) & 0xFF);
  data.push_back(jtSize & 0xFF);

  // Jump table offset from A5
  data.push_back((JT_OFFSET >> 24) & 0xFF);
  data.push_back((JT_OFFSET >> 16) & 0xFF);
  data.push_back((JT_OFFSET >> 8) & 0xFF);
  data.push_back(JT_OFFSET & 0xFF);

  // Jump table entries (8 bytes each, unloaded format):
  // +0: Offset of routine from start of segment (2 bytes)
  // +2: MOVE.W #segment, -(SP) instruction [3F3C XXXX] (4 bytes)
  // +6: _LoadSeg trap [A9F0] (2 bytes)
  //
  // When loaded, this becomes:
  // +0: Segment number (2 bytes)
  // +2: JMP abs.L [4EF9 XXXXXXXX] (6 bytes)

  for (const auto &entry : entries) {
    // Offset in segment (2 bytes)
    data.push_back((entry.offsetInSegment >> 8) & 0xFF);
    data.push_back(entry.offsetInSegment & 0xFF);

    // MOVE.W #segment, -(SP) = 0x3F3C followed by segment number
    data.push_back(0x3F);
    data.push_back(0x3C);
    data.push_back((entry.segmentNum >> 8) & 0xFF);
    data.push_back(entry.segmentNum & 0xFF);

    // _LoadSeg trap = 0xA9F0
    data.push_back(0xA9);
    data.push_back(0xF0);
  }

  return data;
}

} // namespace lld::classic68k
