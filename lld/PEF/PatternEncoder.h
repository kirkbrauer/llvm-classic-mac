//===- PatternEncoder.h - PEF Pattern-Init Data Encoder ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Encodes data sections using PEF pattern-initialized data opcodes.
// Implements Zero and BlockCopy opcodes per Apple's PEF specification.
//
// Reference: Apple's "Mac OS Runtime Architectures", Chapter 8
// https://preterhuman.net/macstuff/techpubs/mac/runtimehtml/RTArch-94.html
//
//===----------------------------------------------------------------------===//

#ifndef LLD_PEF_PATTERN_ENCODER_H
#define LLD_PEF_PATTERN_ENCODER_H

#include "llvm/ADT/ArrayRef.h"
#include <vector>

namespace lld::pef {

/// Encodes data using PEF pattern-init opcodes
/// Reference: PEF_FORMAT_SPECIFICATION.md lines 226-350
class PatternEncoder {
public:
  /// Encode data using pattern-init opcodes
  /// Returns encoded bytecode (may be larger than input for small non-zero data)
  static std::vector<uint8_t> encode(llvm::ArrayRef<uint8_t> data);

  /// Check if pattern encoding would save space
  static bool isBeneficial(llvm::ArrayRef<uint8_t> data);

private:
  /// Detect if data is all zeros
  static bool isAllZeros(llvm::ArrayRef<uint8_t> data);

  /// Encode zero-filled region (Opcode 000)
  static void encodeZero(std::vector<uint8_t> &out, size_t count);

  /// Encode literal data (Opcode 001: BlockCopy)
  static void encodeBlockCopy(std::vector<uint8_t> &out,
                              llvm::ArrayRef<uint8_t> data);

  /// Encode multi-byte count argument (for counts > 31)
  static void encodeCount(std::vector<uint8_t> &out, uint32_t count);
};

} // namespace lld::pef

#endif // LLD_PEF_PATTERN_ENCODER_H
