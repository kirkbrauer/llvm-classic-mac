//===- CodeSegment.h - Classic 68K code segment (CODE n) ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file generates CODE n resources containing executable code for
// classic 68K applications.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_CLASSIC68K_CODESEGMENT_H
#define LLD_CLASSIC68K_CODESEGMENT_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <string>
#include <vector>

namespace lld::classic68k {

// Information about a function in a code segment
struct FunctionInfo {
  std::string name;
  uint32_t offset;    // Offset from start of code (after header)
  uint32_t size;
  bool isEntry;       // Is this an entry in the jump table?
  uint16_t jtIndex;   // Jump table entry index (if isEntry)
};

class CodeSegment {
public:
  CodeSegment(uint16_t segmentNum, llvm::StringRef name = "")
      : segNum(segmentNum), segName(name.str()) {}

  // Set the raw code bytes
  void setCode(llvm::ArrayRef<uint8_t> code);

  // Add function information
  void addFunction(const std::string &name, uint32_t offset, uint32_t size,
                   bool isEntry = false, uint16_t jtIndex = 0);

  // Get segment number
  uint16_t getSegmentNum() const { return segNum; }

  // Get segment name
  llvm::StringRef getName() const { return segName; }

  // Get functions
  const std::vector<FunctionInfo> &getFunctions() const { return functions; }

  // Get raw code size (without header)
  size_t getCodeSize() const { return codeBytes.size(); }

  // Generate the CODE n resource data (with header)
  std::vector<uint8_t> generate(uint16_t firstJTOffset,
                                 uint16_t numJTEntries) const;

  // Near model segment header size
  static constexpr size_t HEADER_SIZE = 4;

private:
  uint16_t segNum;
  std::string segName;
  std::vector<uint8_t> codeBytes;
  std::vector<FunctionInfo> functions;
};

} // namespace lld::classic68k

#endif // LLD_CLASSIC68K_CODESEGMENT_H
