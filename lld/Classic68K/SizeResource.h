//===- SizeResource.h - Classic 68K SIZE resource ----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file generates the SIZE resource (ID -1) specifying memory requirements
// for classic 68K applications.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_CLASSIC68K_SIZERESOURCE_H
#define LLD_CLASSIC68K_SIZERESOURCE_H

#include <cstdint>
#include <vector>

namespace lld::classic68k {

// SIZE resource flags
enum SizeFlags : uint16_t {
  // Bits 15-12: Reserved
  kSaveScreen = 0x0800,           // Save screen before FKEY
  kAcceptSuspendEvents = 0x4000,  // Accepts suspend events
  kDoesActivateOnFGSwitch = 0x0800, // Activate on foreground switch
  kOnlyBackground = 0x0400,       // Only run in background
  kGetFrontClicks = 0x0200,       // Accept front clicks
  kAcceptAppDiedEvents = 0x0100,  // Accept appdied events
  kIs32BitCompatible = 0x0080,    // 32-bit clean
  kHighLevelEventAware = 0x0040,  // Supports Apple Events
  kOnlyLocalHLEvents = 0x0020,    // Only local high-level events
  kStationeryAware = 0x0010,      // Can open stationery
  kUseTextEditServices = 0x0008,  // Uses Text Services Manager
  // Bits 2-0: Reserved

  // Common combinations
  kDefaultFlags = kAcceptSuspendEvents | kDoesActivateOnFGSwitch |
                  kIs32BitCompatible | kHighLevelEventAware, // 0x58C0
};

class SizeResource {
public:
  SizeResource() = default;

  // Set the SIZE flags
  void setFlags(uint16_t f) { flags = f; }

  // Set preferred memory size (in bytes)
  void setPreferredSize(uint32_t size) { preferredSize = size; }

  // Set minimum memory size (in bytes)
  void setMinimumSize(uint32_t size) { minimumSize = size; }

  // Generate the SIZE -1 resource data
  std::vector<uint8_t> generate() const;

private:
  uint16_t flags = kDefaultFlags;
  uint32_t preferredSize = 393216;  // 384KB default
  uint32_t minimumSize = 393216;    // 384KB default
};

} // namespace lld::classic68k

#endif // LLD_CLASSIC68K_SIZERESOURCE_H
