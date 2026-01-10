//===- DataSection.cpp - Classic 68K data section (DATA 0) ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "DataSection.h"

namespace lld::classic68k {

void DataSection::setData(llvm::ArrayRef<uint8_t> data) {
  dataBytes.assign(data.begin(), data.end());
}

uint32_t DataSection::getBelowA5Size() const {
  // Below A5 = app globals + BSS + QuickDraw globals
  return dataBytes.size() + bssSize + QD_GLOBALS_SIZE;
}

std::vector<uint8_t> DataSection::generate() const {
  // DATA 0 resource contains the initialized data that will be copied
  // to the application's A5 world at launch time.
  //
  // The format is simply the raw initialized data bytes.
  // BSS is not stored - it's implicitly zeroed.

  std::vector<uint8_t> data;
  data.insert(data.end(), dataBytes.begin(), dataBytes.end());
  return data;
}

} // namespace lld::classic68k
