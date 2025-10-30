//===- RelocWriter.h - PEF Relocation Bytecode Generator -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares the PEFRelocWriter class for generating PEF relocation
// instruction bytecode.
//
//===----------------------------------------------------------------------===//

#ifndef LLD_PEF_RELOC_WRITER_H
#define LLD_PEF_RELOC_WRITER_H

#include "lld/Common/LLVM.h"
#include "llvm/BinaryFormat/PEF.h"
#include <vector>

namespace lld::pef {

class InputSection;
class OutputSection;
class Symbol;
class Undefined;

// Structure to track imported library information
struct ImportedLibraryInfo {
  StringRef name;
  std::vector<Undefined *> symbols;
  uint32_t nameOffset = 0;
  uint32_t firstImportedSymbol = 0;
};

/// Generates PEF relocation bytecode instructions
class PEFRelocWriter {
public:
  PEFRelocWriter(const std::vector<OutputSection *> &sections,
                 const std::vector<ImportedLibraryInfo> &imports);

  /// Generate relocation headers and instructions
  /// Returns pair of: <headers_bytes, instructions_bytes>
  std::pair<std::vector<uint8_t>, std::vector<uint8_t>> generate();

private:
  // State machine variables
  uint32_t relocAddress = 0;
  int16_t sectionC = -1;  // Code section index
  int16_t sectionD = -1;  // Data section index

  // Output buffers
  std::vector<uint16_t> instructions;
  std::vector<llvm::PEF::LoaderRelocationHeader> headers;

  // Input data
  const std::vector<OutputSection *> &outputSections;
  const std::vector<ImportedLibraryInfo> &importedLibraries;

  // Helper methods - emit instructions
  void emitInstruction(uint16_t instr);
  void emitSetPosition(uint32_t offset);
  void emitBySectC(uint16_t runLength);
  void emitBySectD(uint16_t runLength);
  void emitByImport(uint32_t index);
  void emitSetSectC(uint16_t index);
  void emitSetSectD(uint16_t index);

  /// Process one output section's relocations
  void processSection(OutputSection *osec, unsigned sectionIndex);

  /// Get import index for a symbol
  uint32_t getImportIndex(const Symbol *sym) const;

  /// Optimize instruction stream (Phase 3.3)
  void optimize();
};

} // namespace lld::pef

#endif // LLD_PEF_RELOC_WRITER_H
