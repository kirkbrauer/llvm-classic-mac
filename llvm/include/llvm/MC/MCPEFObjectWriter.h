//===-- llvm/MC/MCPEFObjectWriter.h - PEF Object Writer ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares the MCPEFObjectTargetWriter and PEFObjectWriter classes
// for writing PEF (Preferred Executable Format) object files used by Classic
// Mac OS.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_MC_MCPEFOBJECTWRITER_H
#define LLVM_MC_MCPEFOBJECTWRITER_H

#include "llvm/MC/MCObjectWriter.h"
#include "llvm/TargetParser/Triple.h"
#include <memory>

namespace llvm {

class raw_pwrite_stream;
class MCFixup;
class MCValue;

/// Target-specific PEF object writer interface.
///
/// This abstract class provides the target-specific interface needed by
/// PEFObjectWriter to generate relocations and other target-specific data.
class MCPEFObjectTargetWriter : public MCObjectTargetWriter {
protected:
  /// Architecture type for PEF container (e.g., 'pwpc' for PowerPC).
  uint32_t ArchType;

  MCPEFObjectTargetWriter(uint32_t ArchType);

public:
  ~MCPEFObjectTargetWriter() override;

  Triple::ObjectFormatType getFormat() const override { return Triple::PEF; }

  static bool classof(const MCObjectTargetWriter *W) {
    return W->getFormat() == Triple::PEF;
  }

  uint32_t getArchType() const { return ArchType; }

  /// Get the PEF relocation type and parameters for a given fixup.
  ///
  /// \param Target The relocation target value.
  /// \param Fixup The fixup to be applied.
  /// \param IsPCRel Whether this is a PC-relative relocation.
  /// \returns A pair of (relocation opcode, relocation flags).
  virtual std::pair<uint16_t, uint16_t>
  getRelocTypeAndFlags(const MCValue &Target, const MCFixup &Fixup,
                       bool IsPCRel) const = 0;
};

/// Concrete PEF object writer implementation.
///
/// This class implements the actual PEF file format writing, including:
/// - Container headers
/// - Section headers and data
/// - Loader section with import/export tables
/// - Relocations in PEF's compact bytecode format
class PEFObjectWriter : public MCObjectWriter {
public:
  /// Stored relocation information for later processing
  struct StoredRelocation {
    const MCSection *Section;  // Section containing the relocation
    uint64_t Offset;           // Offset within section
    const MCSymbol *Symbol;    // Target symbol
    uint16_t Type;             // PEF relocation type
    uint16_t Flags;            // Relocation flags
    int64_t Addend;            // Addend value
  };

private:
  std::unique_ptr<MCPEFObjectTargetWriter> TargetObjectWriter;
  raw_pwrite_stream &OS;
  bool IsLittleEndian;

  /// List of relocations collected during assembly
  std::vector<StoredRelocation> Relocations;

public:
  PEFObjectWriter(std::unique_ptr<MCPEFObjectTargetWriter> MOTW,
                  raw_pwrite_stream &OS, bool IsLittleEndian);

  ~PEFObjectWriter() override;

  void reset() override;

  void executePostLayoutBinding(MCAssembler &Asm) override;

  void recordRelocation(MCAssembler &Asm, const MCFragment *Fragment,
                        const MCFixup &Fixup, MCValue Target,
                        uint64_t &FixedValue) override;

  uint64_t writeObject(MCAssembler &Asm) override;

  bool isSymbolRefDifferenceFullyResolvedImpl(const MCAssembler &Asm,
                                              const MCSymbol &SymA,
                                              const MCFragment &FB, bool InSet,
                                              bool IsPCRel) const override;

  /// Get the list of relocations for passing to PEFWriter
  const std::vector<StoredRelocation> &getRelocations() const {
    return Relocations;
  }
};

/// Factory function to create a PEF object writer.
///
/// \param MOTW Target-specific PEF writer.
/// \param OS Output stream for the PEF file.
/// \param IsLittleEndian Endianness (typically false for PowerPC).
/// \returns A unique pointer to the created PEF object writer.
std::unique_ptr<MCObjectWriter>
createPEFObjectWriter(std::unique_ptr<MCPEFObjectTargetWriter> MOTW,
                      raw_pwrite_stream &OS, bool IsLittleEndian);

} // end namespace llvm

#endif // LLVM_MC_MCPEFOBJECTWRITER_H
