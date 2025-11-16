//===- PatternEncoder.cpp - PEF Pattern-Init Data Encoder -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "PatternEncoder.h"
#include "llvm/Support/ErrorHandling.h"
#include <algorithm>

using namespace llvm;
using namespace lld::pef;

bool PatternEncoder::isAllZeros(ArrayRef<uint8_t> data) {
  return std::all_of(data.begin(), data.end(), [](uint8_t b) { return b == 0; });
}

bool PatternEncoder::isBeneficial(ArrayRef<uint8_t> data) {
  if (data.empty())
    return false;

  // All-zero data: 1 byte opcode vs data.size() bytes raw
  if (isAllZeros(data))
    return data.size() > 1;

  // BlockCopy: 1 byte opcode + data.size() bytes
  // Only beneficial if original data would need alignment/padding
  return false;  // For non-zero data, no savings in most cases
}

void PatternEncoder::encodeCount(std::vector<uint8_t> &out, uint32_t count) {
  // Variable-length encoding (big-endian)
  // Bit 7 = continuation flag, bits 6-0 = data
  // Per PEF spec lines 280-293

  if (count <= 0x7F) {
    // Single-byte encoding
    out.push_back(static_cast<uint8_t>(count));
    return;
  }

  // Multi-byte encoding
  std::vector<uint8_t> bytes;

  do {
    uint8_t byte = count & 0x7F;
    count >>= 7;
    if (count > 0)
      byte |= 0x80;  // Set continuation bit
    bytes.push_back(byte);
  } while (count > 0);

  // Reverse to big-endian order
  std::reverse(bytes.begin(), bytes.end());
  out.insert(out.end(), bytes.begin(), bytes.end());
}

void PatternEncoder::encodeZero(std::vector<uint8_t> &out, size_t count) {
  // Opcode 000 (Zero): bits 7-5 = 000, bits 4-0 = count
  // Per PEF spec lines 258-261

  if (count <= 31) {
    // Single-byte encoding: opcode 000 + 5-bit count
    out.push_back(static_cast<uint8_t>(count));
  } else {
    // Multi-byte encoding: opcode 000 + count=0 + extended count
    out.push_back(0x00);  // Opcode 000, count field = 0 (signals extended)
    encodeCount(out, count);
  }
}

void PatternEncoder::encodeBlockCopy(std::vector<uint8_t> &out,
                                     ArrayRef<uint8_t> data) {
  // Opcode 001 (BlockCopy): bits 7-5 = 001, bits 4-0 = count
  // Per PEF spec lines 263-265

  size_t count = data.size();

  if (count <= 31) {
    // Single-byte encoding: opcode 001 (0x20) + 5-bit count
    out.push_back(0x20 | static_cast<uint8_t>(count));
  } else {
    // Multi-byte encoding: opcode 001 + count=0 + extended count
    out.push_back(0x20);  // Opcode 001, count field = 0
    encodeCount(out, count);
  }

  // Append literal data bytes
  out.insert(out.end(), data.begin(), data.end());
}

std::vector<uint8_t> PatternEncoder::encode(ArrayRef<uint8_t> data) {
  std::vector<uint8_t> result;

  if (data.empty())
    return result;

  // Enhanced strategy: detect runs of zeros and non-zero regions
  // This matches CodeWarrior's efficient encoding approach

  size_t i = 0;
  while (i < data.size()) {
    // Count consecutive zeros
    size_t zeroStart = i;
    while (i < data.size() && data[i] == 0)
      i++;

    size_t zeroCount = i - zeroStart;

    // Emit Zero opcode for runs of 4+ zeros (saves space)
    if (zeroCount >= 4) {
      encodeZero(result, zeroCount);
    } else if (zeroCount > 0) {
      // Small run of zeros - include in next BlockCopy
      i = zeroStart;
    }

    // Count consecutive non-zeros (or small zero runs)
    size_t dataStart = i;
    while (i < data.size()) {
      // Lookahead: if we see 4+ consecutive zeros, stop here
      size_t lookahead = i;
      size_t zeroRun = 0;
      while (lookahead < data.size() && data[lookahead] == 0) {
        zeroRun++;
        lookahead++;
      }

      if (zeroRun >= 4)
        break;  // Stop before this zero run

      // Otherwise include this byte (and any small zero run)
      i = lookahead > i ? lookahead : i + 1;
    }

    size_t dataCount = i - dataStart;
    if (dataCount > 0) {
      encodeBlockCopy(result, data.slice(dataStart, dataCount));
    }
  }

  return result;
}
