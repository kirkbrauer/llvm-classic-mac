//===- ResourceWriter.cpp - Mac resource fork writer ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ResourceWriter.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <map>

using namespace llvm;
using namespace llvm::support::endian;

namespace lld::classic68k {

uint32_t ResourceWriter::makeType(const char *s) {
  return (uint32_t(s[0]) << 24) | (uint32_t(s[1]) << 16) |
         (uint32_t(s[2]) << 8) | uint32_t(s[3]);
}

void ResourceWriter::addResource(uint32_t type, int16_t id,
                                  ArrayRef<uint8_t> data, uint8_t attrs,
                                  StringRef name) {
  Resource res;
  res.type = type;
  res.id = id;
  res.name = name.str();
  res.attrs = attrs;
  res.data.assign(data.begin(), data.end());
  resources.push_back(std::move(res));
}

void ResourceWriter::addResource(const char *typeName, int16_t id,
                                  ArrayRef<uint8_t> data, uint8_t attrs,
                                  StringRef name) {
  addResource(makeType(typeName), id, data, attrs, name);
}

std::vector<uint8_t> ResourceWriter::generate() const {
  // Resource fork structure:
  // - Header (256 bytes)
  // - Data area (resource data)
  // - Map area (type list, reference list, name list)

  // Group resources by type
  std::map<uint32_t, std::vector<const Resource *>> byType;
  for (const auto &res : resources)
    byType[res.type].push_back(&res);

  // Build data area: each resource is prefixed with 4-byte length
  std::vector<uint8_t> dataArea;
  std::vector<uint32_t> dataOffsets;  // Offset of each resource in data area

  for (const auto &res : resources) {
    dataOffsets.push_back(dataArea.size());
    // 4-byte length prefix
    uint32_t len = res.data.size();
    dataArea.push_back((len >> 24) & 0xFF);
    dataArea.push_back((len >> 16) & 0xFF);
    dataArea.push_back((len >> 8) & 0xFF);
    dataArea.push_back(len & 0xFF);
    // Resource data
    dataArea.insert(dataArea.end(), res.data.begin(), res.data.end());
  }

  // Pad data area to 4-byte boundary
  while (dataArea.size() % 4 != 0)
    dataArea.push_back(0);

  // Build name list
  std::vector<uint8_t> nameList;
  std::map<const Resource *, uint16_t> nameOffsets;
  for (const auto &res : resources) {
    if (!res.name.empty()) {
      nameOffsets[&res] = nameList.size();
      // Pascal string: length byte + characters
      nameList.push_back(res.name.size());
      nameList.insert(nameList.end(), res.name.begin(), res.name.end());
    }
  }

  // Build type list and reference list
  std::vector<uint8_t> typeList;
  std::vector<uint8_t> refList;

  // Type list starts with count - 1
  uint16_t numTypes = byType.size();
  typeList.push_back((numTypes - 1) >> 8);
  typeList.push_back((numTypes - 1) & 0xFF);

  // Reference list offset from type list start
  // Type list = 2 + (8 * numTypes) bytes
  // Each type entry: 4 (type) + 2 (count-1) + 2 (ref offset)
  uint16_t refListOffset = 2 + 8 * numTypes;

  size_t resIndex = 0;
  for (const auto &[type, resList] : byType) {
    // Type entry: 4-byte type code
    typeList.push_back((type >> 24) & 0xFF);
    typeList.push_back((type >> 16) & 0xFF);
    typeList.push_back((type >> 8) & 0xFF);
    typeList.push_back(type & 0xFF);

    // Count - 1
    uint16_t count = resList.size();
    typeList.push_back((count - 1) >> 8);
    typeList.push_back((count - 1) & 0xFF);

    // Reference list offset (from type list start)
    uint16_t offset = refListOffset + refList.size();
    typeList.push_back(offset >> 8);
    typeList.push_back(offset & 0xFF);

    // Add reference entries for this type
    for (const Resource *res : resList) {
      // Resource ID (2 bytes, signed)
      refList.push_back((res->id >> 8) & 0xFF);
      refList.push_back(res->id & 0xFF);

      // Name offset (2 bytes, 0xFFFF if no name)
      if (res->name.empty()) {
        refList.push_back(0xFF);
        refList.push_back(0xFF);
      } else {
        uint16_t nameOff = nameOffsets[res];
        refList.push_back(nameOff >> 8);
        refList.push_back(nameOff & 0xFF);
      }

      // Attributes (1 byte)
      refList.push_back(res->attrs);

      // Data offset (3 bytes)
      uint32_t dataOff = dataOffsets[resIndex];
      refList.push_back((dataOff >> 16) & 0xFF);
      refList.push_back((dataOff >> 8) & 0xFF);
      refList.push_back(dataOff & 0xFF);

      // Handle placeholder (4 bytes, 0 in file)
      refList.push_back(0);
      refList.push_back(0);
      refList.push_back(0);
      refList.push_back(0);

      resIndex++;
    }
  }

  // Build resource map
  std::vector<uint8_t> resMap;

  // Map header (28 bytes):
  // - Copy of data offset (4)
  // - Copy of map offset (4)
  // - Copy of data length (4)
  // - Copy of map length (4) - will be patched later
  // - Next resource map handle (4)
  // - File ref num (2)
  // - Resource file attributes (2)
  // - Type list offset from map start (2)
  // - Name list offset from map start (2)

  // Calculate sizes for the header copies
  // We need to know the total map size, but it depends on nameList which we haven't
  // fully calculated yet. We'll calculate the map size first.
  //
  // resMap size = 28 (header) + typeList.size() + refList.size() + nameList.size()
  size_t mapSize = 28 + typeList.size() + refList.size() + nameList.size();

  uint32_t dataOffset = 256;  // After header
  uint32_t mapOffset = dataOffset + dataArea.size();
  uint32_t dataLength = dataArea.size();
  uint32_t mapLength = mapSize;

  // Copy of data offset (4 bytes)
  resMap.push_back((dataOffset >> 24) & 0xFF);
  resMap.push_back((dataOffset >> 16) & 0xFF);
  resMap.push_back((dataOffset >> 8) & 0xFF);
  resMap.push_back(dataOffset & 0xFF);

  // Copy of map offset (4 bytes)
  resMap.push_back((mapOffset >> 24) & 0xFF);
  resMap.push_back((mapOffset >> 16) & 0xFF);
  resMap.push_back((mapOffset >> 8) & 0xFF);
  resMap.push_back(mapOffset & 0xFF);

  // Copy of data length (4 bytes)
  resMap.push_back((dataLength >> 24) & 0xFF);
  resMap.push_back((dataLength >> 16) & 0xFF);
  resMap.push_back((dataLength >> 8) & 0xFF);
  resMap.push_back(dataLength & 0xFF);

  // Copy of map length (4 bytes)
  resMap.push_back((mapLength >> 24) & 0xFF);
  resMap.push_back((mapLength >> 16) & 0xFF);
  resMap.push_back((mapLength >> 8) & 0xFF);
  resMap.push_back(mapLength & 0xFF);

  // Next resource map handle (4 bytes, 0 in file)
  for (int i = 0; i < 4; i++)
    resMap.push_back(0);

  // File ref num (2 bytes, 0 in file)
  resMap.push_back(0);
  resMap.push_back(0);

  // Resource file attributes (2 bytes)
  resMap.push_back(0);
  resMap.push_back(0);

  // Type list offset from map start (always 28)
  resMap.push_back(0);
  resMap.push_back(28);

  // Name list offset from map start
  uint16_t nameListOffset = 28 + typeList.size() + refList.size();
  resMap.push_back(nameListOffset >> 8);
  resMap.push_back(nameListOffset & 0xFF);

  // Append type list and reference list
  resMap.insert(resMap.end(), typeList.begin(), typeList.end());
  resMap.insert(resMap.end(), refList.begin(), refList.end());

  // Append name list
  resMap.insert(resMap.end(), nameList.begin(), nameList.end());

  // Build complete resource fork
  std::vector<uint8_t> fork;

  // Header (256 bytes)
  // Data offset (4 bytes) - reuse values calculated above
  fork.push_back((dataOffset >> 24) & 0xFF);
  fork.push_back((dataOffset >> 16) & 0xFF);
  fork.push_back((dataOffset >> 8) & 0xFF);
  fork.push_back(dataOffset & 0xFF);

  // Map offset (4 bytes)
  fork.push_back((mapOffset >> 24) & 0xFF);
  fork.push_back((mapOffset >> 16) & 0xFF);
  fork.push_back((mapOffset >> 8) & 0xFF);
  fork.push_back(mapOffset & 0xFF);

  // Data length (4 bytes)
  fork.push_back((dataLength >> 24) & 0xFF);
  fork.push_back((dataLength >> 16) & 0xFF);
  fork.push_back((dataLength >> 8) & 0xFF);
  fork.push_back(dataLength & 0xFF);

  // Map length (4 bytes)
  fork.push_back((mapLength >> 24) & 0xFF);
  fork.push_back((mapLength >> 16) & 0xFF);
  fork.push_back((mapLength >> 8) & 0xFF);
  fork.push_back(mapLength & 0xFF);

  // Rest of header is zeros
  fork.resize(256, 0);

  // Data area
  fork.insert(fork.end(), dataArea.begin(), dataArea.end());

  // Resource map
  fork.insert(fork.end(), resMap.begin(), resMap.end());

  return fork;
}

bool ResourceWriter::writeToFile(StringRef path) const {
  // On macOS, write to the resource fork via extended attribute
  std::string rsrcPath = (path + "/..namedfork/rsrc").str();

  std::error_code ec;
  raw_fd_ostream os(rsrcPath, ec, sys::fs::OF_None);
  if (ec)
    return false;

  auto data = generate();
  os.write(reinterpret_cast<const char *>(data.data()), data.size());
  return !os.has_error();
}

bool ResourceWriter::writeToRsrcFile(StringRef path) const {
  std::error_code ec;
  raw_fd_ostream os(path, ec, sys::fs::OF_None);
  if (ec)
    return false;

  auto data = generate();
  os.write(reinterpret_cast<const char *>(data.data()), data.size());
  return !os.has_error();
}

} // namespace lld::classic68k
