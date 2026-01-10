//===- ResourceWriter.h - Mac resource fork writer ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file generates Mac OS resource forks containing CODE, DATA, and SIZE
// resources for classic 68K applications.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_CLASSIC68K_RESOURCEWRITER_H
#define LLD_CLASSIC68K_RESOURCEWRITER_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Endian.h"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace lld::classic68k {

// Resource attributes
enum ResourceAttr : uint8_t {
  resSysHeap = 0x40,    // Load in system heap
  resPurgeable = 0x20,  // Purgeable
  resLocked = 0x10,     // Locked
  resProtected = 0x08,  // Protected
  resPreload = 0x04,    // Load at launch
  resChanged = 0x02,    // Changed (internal use)
};

// A single resource entry
struct Resource {
  uint32_t type;        // 4-character type code (e.g., 'CODE', 'DATA')
  int16_t id;           // Resource ID
  std::string name;     // Optional name
  uint8_t attrs;        // Attributes (purgeable, protected, etc.)
  std::vector<uint8_t> data;  // Resource data
};

class ResourceWriter {
public:
  ResourceWriter() = default;

  // Add a resource
  void addResource(uint32_t type, int16_t id, llvm::ArrayRef<uint8_t> data,
                   uint8_t attrs = 0, llvm::StringRef name = "");

  // Helper to add resources by type name
  void addResource(const char *typeName, int16_t id,
                   llvm::ArrayRef<uint8_t> data, uint8_t attrs = 0,
                   llvm::StringRef name = "");

  // Generate the complete resource fork
  std::vector<uint8_t> generate() const;

  // Write to file's resource fork (macOS extended attribute)
  bool writeToFile(llvm::StringRef path) const;

  // Write to standalone .rsrc file
  bool writeToRsrcFile(llvm::StringRef path) const;

private:
  std::vector<Resource> resources;

  // Convert 4-char string to uint32_t type code
  static uint32_t makeType(const char *s);
};

} // namespace lld::classic68k

#endif // LLD_CLASSIC68K_RESOURCEWRITER_H
