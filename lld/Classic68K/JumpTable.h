//===- JumpTable.h - Classic 68K jump table (CODE 0) -------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file generates the CODE 0 resource containing the jump table for
// classic 68K applications.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_CLASSIC68K_JUMPTABLE_H
#define LLD_CLASSIC68K_JUMPTABLE_H

#include "llvm/ADT/SmallVector.h"
#include <cstdint>
#include <string>
#include <vector>

namespace lld::classic68k {

// A jump table entry for an exported/called function
struct JumpTableEntry {
  std::string name;           // Symbol name
  uint16_t segmentNum;        // CODE segment number (1-based)
  uint16_t offsetInSegment;   // Offset of function from segment start
};

class JumpTable {
public:
  JumpTable() = default;

  // Add an entry to the jump table
  void addEntry(const std::string &name, uint16_t segment, uint16_t offset);

  // Get the number of entries
  size_t size() const { return entries.size(); }

  // Get entry by index
  const JumpTableEntry &getEntry(size_t idx) const { return entries[idx]; }

  // Set sizes for the A5 world
  void setAboveA5Size(uint32_t size) { aboveA5 = size; }
  void setBelowA5Size(uint32_t size) { belowA5 = size; }

  // Generate the CODE 0 resource data
  std::vector<uint8_t> generate() const;

  // Get the calculated above/below A5 sizes
  uint32_t getAboveA5Size() const;
  uint32_t getBelowA5Size() const { return belowA5; }

  // Jump table offset from A5 (always 32 for standard apps)
  static constexpr uint32_t JT_OFFSET = 32;

  // Size of each jump table entry in bytes
  static constexpr size_t ENTRY_SIZE = 8;

private:
  std::vector<JumpTableEntry> entries;
  uint32_t aboveA5 = 0;  // Set by caller based on actual needs
  uint32_t belowA5 = 0;  // Global data size + QuickDraw globals
};

} // namespace lld::classic68k

#endif // LLD_CLASSIC68K_JUMPTABLE_H
