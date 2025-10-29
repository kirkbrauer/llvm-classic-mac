//===- MCSectionPEF.h - PEF Machine Code Section ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares the MCSectionPEF class.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_MC_MCSECTIONPEF_H
#define LLVM_MC_MCSECTIONPEF_H

#include "llvm/MC/MCSection.h"
#include "llvm/MC/SectionKind.h"

namespace llvm {

class MCSymbol;

/// Represents a section in the PEF (Preferred Executable Format) for
/// Classic Mac OS. PEF sections contain code, data, or loader information.
class MCSectionPEF final : public MCSection {
  friend class MCContext;

  /// Section type - corresponds to PEF section kinds
  /// 0 = Code, 1 = Data, 2 = Pattern-initialized data, 3 = Constant, 4 = Loader
  unsigned SectionType;

  MCSectionPEF(StringRef Name, SectionKind K, unsigned Type, MCSymbol *Begin)
      : MCSection(SV_PEF, Name, K.isText(), /*IsVirtual=*/false, Begin),
        SectionType(Type) {}

public:
  void printSwitchToSection(const MCAsmInfo &, const Triple &, raw_ostream &,
                            uint32_t) const override;
  bool useCodeAlign() const override { return SectionType == 0; } // Code sections use alignment

  unsigned getSectionType() const { return SectionType; }

  static bool classof(const MCSection *S) { return S->getVariant() == SV_PEF; }
};

} // end namespace llvm

#endif // LLVM_MC_MCSECTIONPEF_H
