//===-- PPCPEFObjectWriter.cpp - PowerPC PEF Writer ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the PowerPC-specific PEF object writer for Classic
// Mac OS targets.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/PPCFixupKinds.h"
#include "MCTargetDesc/PPCMCTargetDesc.h"
#include "llvm/BinaryFormat/PEF.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCPEFObjectWriter.h"
#include "llvm/MC/MCValue.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

namespace {

class PPCPEFObjectWriter : public MCPEFObjectTargetWriter {
public:
  PPCPEFObjectWriter();

  std::pair<uint16_t, uint16_t>
  getRelocTypeAndFlags(const MCValue &Target, const MCFixup &Fixup,
                       bool IsPCRel) const override;
};

} // end anonymous namespace

PPCPEFObjectWriter::PPCPEFObjectWriter()
    : MCPEFObjectTargetWriter(PEF::kPEFPowerPCArch) {}

std::unique_ptr<MCObjectTargetWriter> llvm::createPPCPEFObjectWriter() {
  return std::make_unique<PPCPEFObjectWriter>();
}

std::pair<uint16_t, uint16_t> PPCPEFObjectWriter::getRelocTypeAndFlags(
    const MCValue &Target, const MCFixup &Fixup, bool IsPCRel) const {
  const MCSymbolRefExpr::VariantKind Modifier =
      Target.isAbsolute() ? MCSymbolRefExpr::VK_None
                          : Target.getSymA()->getKind();

  // Map PowerPC fixup kinds to PEF relocation opcodes
  switch (static_cast<unsigned>(Fixup.getKind())) {
  default:
    report_fatal_error("Unimplemented fixup kind for PEF.");

  case PPC::fixup_ppc_br24:
  case PPC::fixup_ppc_br24abs:
    // Branch relocations - these are PC-relative
    // Use kPEFRelocBySectC for code section relocations
    return {PEF::kPEFRelocBySectC, 0};

  case PPC::fixup_ppc_half16:
  case PPC::fixup_ppc_half16ds:
  case PPC::fixup_ppc_half16dq:
    // Half-word relocations (16-bit)
    switch (Modifier) {
    default:
      report_fatal_error("Unsupported modifier for half16 fixup in PEF.");
    case MCSymbolRefExpr::VK_None:
    case MCSymbolRefExpr::VK_PPC_U:
    case MCSymbolRefExpr::VK_PPC_L:
      // Use section-relative relocation
      return {PEF::kPEFRelocBySectC, 0};
    }
    break;

  case FK_Data_4:
    // 32-bit data relocation
    // Choose relocation type based on target section
    if (IsPCRel)
      return {PEF::kPEFRelocBySectC, 0}; // PC-relative code relocation
    else
      return {PEF::kPEFRelocBySectD, 0}; // Data section relocation

  case FK_Data_8:
    // 64-bit data relocation (not common in 32-bit Classic Mac OS)
    return {PEF::kPEFRelocBySectD, 0};

  case PPC::fixup_ppc_nofixup:
    // No fixup required - just a reference
    return {0, 0};
  }
}
