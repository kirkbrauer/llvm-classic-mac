//===- lib/MC/MCPEFStreamer.cpp - PEF Object Output ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file assembles .s files and emits PEF (Preferred Executable Format)
// object files for Classic Mac OS targets.
//
//===----------------------------------------------------------------------===//

#include "llvm/MC/MCPEFStreamer.h"
#include "llvm/BinaryFormat/PEF.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAssembler.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCFragment.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCSection.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/MC/MCValue.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

MCPEFStreamer::MCPEFStreamer(MCContext &Context,
                             std::unique_ptr<MCAsmBackend> MAB,
                             std::unique_ptr<MCObjectWriter> OW,
                             std::unique_ptr<MCCodeEmitter> Emitter)
    : MCObjectStreamer(Context, std::move(MAB), std::move(OW),
                       std::move(Emitter)) {}

void MCPEFStreamer::initSections(bool NoExecStack,
                                 const MCSubtargetInfo &STI) {
  // PEF uses .text for code and .data for data
  switchSection(getContext().getObjectFileInfo()->getTextSection());
}

void MCPEFStreamer::emitLabel(MCSymbol *Symbol, SMLoc Loc) {
  MCObjectStreamer::emitLabel(Symbol, Loc);
}

void MCPEFStreamer::emitAssemblerFlag(MCAssemblerFlag Flag) {
  // PEF doesn't use assembler flags in the same way as ELF/Mach-O
  // Most flags are ignored for PEF
}

void MCPEFStreamer::emitThumbFunc(MCSymbol *Func) {
  // Not applicable to PowerPC/PEF
  llvm_unreachable("Thumb functions are not supported on PowerPC");
}

void MCPEFStreamer::emitWeakReference(MCSymbol *Alias, const MCSymbol *Symbol) {
  // PEF supports weak imports through the loader section
  // For now, treat as a regular reference
  getAssembler().registerSymbol(*Symbol);
}

bool MCPEFStreamer::emitSymbolAttribute(MCSymbol *Symbol,
                                        MCSymbolAttr Attribute) {
  // Handle symbol attributes
  switch (Attribute) {
  case MCSA_Global:
  case MCSA_Extern:
    // Mark symbol as externally visible (will be exported in PEF)
    Symbol->setExternal(true);
    return true;
  case MCSA_Weak:
  case MCSA_WeakReference:
    // PEF supports weak symbols through import options
    Symbol->setExternal(true);
    return true;
  case MCSA_PrivateExtern:
  case MCSA_Hidden:
  case MCSA_Protected:
    // Not externally visible
    return true;
  default:
    return false;
  }
}

void MCPEFStreamer::emitSymbolDesc(MCSymbol *Symbol, unsigned DescValue) {
  // Symbol descriptors are primarily a Mach-O concept
  // Not directly applicable to PEF
}

void MCPEFStreamer::emitCommonSymbol(MCSymbol *Symbol, uint64_t Size,
                                     Align ByteAlignment) {
  // Emit a common symbol (uninitialized data)
  // In PEF, this goes into the .bss section
  MCSection *Section = getContext().getObjectFileInfo()->getBSSSection();

  getAssembler().registerSymbol(*Symbol);
  Symbol->setExternal(true);
  Symbol->setCommon(Size, ByteAlignment);

  switchSection(Section);
  emitValueToAlignment(ByteAlignment, 0, 1, 0);
  emitLabel(Symbol);
  emitZeros(Size);
}

void MCPEFStreamer::emitZerofill(MCSection *Section, MCSymbol *Symbol,
                                 uint64_t Size, Align ByteAlignment,
                                 SMLoc Loc) {
  switchSection(Section);

  if (Symbol) {
    emitValueToAlignment(ByteAlignment);
    emitLabel(Symbol, Loc);
  }

  emitZeros(Size);
}

void MCPEFStreamer::emitInstToData(const MCInst &Inst,
                                   const MCSubtargetInfo &STI) {
  MCAssembler &Assembler = getAssembler();
  SmallVector<MCFixup, 4> Fixups;
  SmallString<256> Code;

  Assembler.getEmitter().encodeInstruction(Inst, Code, Fixups, STI);

  MCDataFragment *DF = getOrCreateDataFragment();

  // Add fixups
  for (MCFixup &Fixup : Fixups) {
    Fixup.setOffset(Fixup.getOffset() + DF->getContents().size());
    DF->getFixups().push_back(Fixup);
  }

  DF->setHasInstructions(STI);
  DF->getContents().append(Code.begin(), Code.end());
}

void MCPEFStreamer::emitBytes(StringRef Data) {
  MCDataFragment *DF = getOrCreateDataFragment();
  DF->getContents().append(Data.begin(), Data.end());
}

void MCPEFStreamer::emitValueImpl(const MCExpr *Value, unsigned Size,
                                  SMLoc Loc) {
  MCDataFragment *DF = getOrCreateDataFragment();
  MCFixup Fixup = MCFixup::create(DF->getContents().size(), Value,
                                  MCFixup::getKindForSize(Size, false), Loc);
  DF->getFixups().push_back(Fixup);
  DF->getContents().resize(DF->getContents().size() + Size, 0);
}

void MCPEFStreamer::finishImpl() {
  // Finalize the PEF object file
  MCObjectStreamer::finishImpl();
}

void MCPEFStreamer::emitInstToFragment(const MCInst &Inst,
                                       const MCSubtargetInfo &STI) {
  // Emit instruction to a relaxable fragment
  // For PEF, we use the same approach as other formats
  MCRelaxableFragment *IF = new MCRelaxableFragment(Inst, STI);
  insert(IF);
}

MCStreamer *llvm::createPEFStreamer(MCContext &Context,
                                    std::unique_ptr<MCAsmBackend> MAB,
                                    std::unique_ptr<MCObjectWriter> OW,
                                    std::unique_ptr<MCCodeEmitter> CE) {
  return new MCPEFStreamer(Context, std::move(MAB), std::move(OW),
                           std::move(CE));
}
