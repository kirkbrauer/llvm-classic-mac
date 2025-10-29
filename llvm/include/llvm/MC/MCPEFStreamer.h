//===-- MCPEFStreamer.h - MCStreamer PEF Object File Interface -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares the MCPEFStreamer class for streaming PEF object files.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_MC_MCPEFSTREAMER_H
#define LLVM_MC_MCPEFSTREAMER_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/MC/MCDirectives.h"
#include "llvm/MC/MCObjectStreamer.h"
#include "llvm/MC/MCPEFObjectWriter.h"
#include "llvm/Support/DataTypes.h"

namespace llvm {

class MCAsmBackend;
class MCCodeEmitter;
class MCContext;
class MCExpr;
class MCInst;

/// Streaming PEF (Preferred Executable Format) object file writer for
/// Classic Mac OS targets.
class MCPEFStreamer : public MCObjectStreamer {
public:
  MCPEFStreamer(MCContext &Context, std::unique_ptr<MCAsmBackend> MAB,
                std::unique_ptr<MCObjectWriter> OW,
                std::unique_ptr<MCCodeEmitter> Emitter);

  ~MCPEFStreamer() override = default;

  /// @name MCStreamer Interface
  /// @{

  void initSections(bool NoExecStack, const MCSubtargetInfo &STI) override;
  void emitLabel(MCSymbol *Symbol, SMLoc Loc = SMLoc()) override;
  void emitAssemblerFlag(MCAssemblerFlag Flag) override;
  void emitThumbFunc(MCSymbol *Func) override;
  void emitWeakReference(MCSymbol *Alias, const MCSymbol *Symbol) override;
  bool emitSymbolAttribute(MCSymbol *Symbol, MCSymbolAttr Attribute) override;
  void emitSymbolDesc(MCSymbol *Symbol, unsigned DescValue) override;
  void emitCommonSymbol(MCSymbol *Symbol, uint64_t Size,
                        Align ByteAlignment) override;

  void emitZerofill(MCSection *Section, MCSymbol *Symbol = nullptr,
                    uint64_t Size = 0, Align ByteAlignment = Align(1),
                    SMLoc Loc = SMLoc()) override;

  void emitInstToData(const MCInst &Inst, const MCSubtargetInfo &) override;
  void emitBytes(StringRef Data) override;
  void emitValueImpl(const MCExpr *Value, unsigned Size,
                     SMLoc Loc = SMLoc()) override;

  void finishImpl() override;

  /// @}

private:
  void emitInstToFragment(const MCInst &Inst, const MCSubtargetInfo &) override;
};

/// Create a new PEF streamer.
MCStreamer *createPEFStreamer(MCContext &Context,
                              std::unique_ptr<MCAsmBackend> MAB,
                              std::unique_ptr<MCObjectWriter> OW,
                              std::unique_ptr<MCCodeEmitter> CE);

} // end namespace llvm

#endif // LLVM_MC_MCPEFSTREAMER_H
