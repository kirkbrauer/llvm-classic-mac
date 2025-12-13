//===- InputSection.h -------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_PEF_INPUT_SECTION_H
#define LLD_PEF_INPUT_SECTION_H

#include "lld/Common/LLVM.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/BinaryFormat/PEF.h"
#include "llvm/Support/Error.h"
#include <cstdint>

namespace lld::pef {

class InputFile;
class ObjFile;
class Symbol;

/// Simple ELF relocation info for InputSection (copied from ELFRelocation)
struct InputSectionReloc {
  uint64_t offset;      // Offset within the section
  uint32_t type;        // ELF relocation type (R_PPC_*)
  Symbol *symbol;       // Resolved symbol
  int64_t addend;       // Addend
};

/// Represents a section from an input object file (PEF or ELF).
/// This class abstracts away the source format, providing a unified
/// interface for the PEF writer.
class InputSection {
public:
  /// Construct from PEF object file (legacy)
  InputSection(ObjFile *file, unsigned index, const llvm::PEF::SectionHeader &hdr)
      : inputFile(reinterpret_cast<InputFile*>(file)),
        sectionIndex(index), header(hdr), fromELF(false) {}

  /// Construct from ELF data (new format)
  InputSection(InputFile *file, unsigned index, StringRef name, uint8_t kind,
               ArrayRef<uint8_t> data, uint32_t align)
      : inputFile(file), sectionIndex(index), fromELF(true),
        elfName(name), elfKind(kind), elfData(data.begin(), data.end()),
        elfAlignment(align) {
    // Initialize header with ELF-derived values
    header.SectionKind = kind;
    header.TotalLength = data.size();
    header.UnpackedLength = data.size();
    header.Alignment = 0;
    // Compute alignment as power of 2
    if (align > 1) {
      uint32_t p = 0;
      while ((1U << p) < align) p++;
      header.Alignment = p;
    }
  }

  /// Get the owning file (generic)
  InputFile *getFile() const { return inputFile; }

  /// Get the owning file as ObjFile (for PEF-specific code)
  ObjFile *getPEFFile() const;

  /// Get the section index in the input file
  unsigned getIndex() const { return sectionIndex; }

  /// Get the section header (for PEF compatibility)
  const llvm::PEF::SectionHeader &getHeader() const { return header; }

  /// Get section kind (code, data, etc.)
  uint8_t getKind() const { return fromELF ? elfKind : header.SectionKind; }

  /// Get section size in memory
  uint64_t getSize() const {
    return fromELF ? elfData.size() : header.TotalLength;
  }

  /// Get unpacked data size
  uint64_t getUnpackedSize() const {
    return fromELF ? elfData.size() : header.UnpackedLength;
  }

  /// Get section data
  Expected<ArrayRef<uint8_t>> getData() const;

  /// Get section name
  StringRef getName() const;

  /// Virtual address assigned during layout (0 if not assigned yet)
  uint64_t getVirtualAddress() const { return virtualAddress; }
  void setVirtualAddress(uint64_t addr) { virtualAddress = addr; }

  /// Alignment requirement (power of 2)
  uint32_t getAlignment() const {
    return fromELF ? elfAlignment : (1U << header.Alignment);
  }

  /// Check if this section came from ELF
  bool isFromELF() const { return fromELF; }

  /// Relocation support (PEF-style 16-bit opcodes)
  ArrayRef<uint16_t> getRelocations() const { return relocInstructions; }
  void setRelocations(ArrayRef<uint16_t> relocs) {
    relocInstructions.assign(relocs.begin(), relocs.end());
  }

  /// ELF relocation support
  ArrayRef<InputSectionReloc> getELFRelocations() const { return elfRelocations; }
  void addELFRelocation(const InputSectionReloc &reloc) {
    elfRelocations.push_back(reloc);
  }

  /// Get mutable ELF relocations (for symbol re-resolution after parsing)
  llvm::SmallVectorImpl<InputSectionReloc> &getMutableELFRelocations() {
    return elfRelocations;
  }

  /// Patched data support (after relocation processing)
  bool hasPatchedData() const { return !patchedData.empty(); }
  ArrayRef<uint8_t> getPatchedData() const { return patchedData; }
  void setPatchedData(std::vector<uint8_t> data) {
    patchedData = std::move(data);
  }

  /// Get mutable reference to patched data (for relocation processing)
  std::vector<uint8_t> &getMutablePatchedData() { return patchedData; }

  /// Initialize patched data from original data
  void initializePatchedData() {
    if (patchedData.empty()) {
      if (fromELF) {
        patchedData = elfData;
      } else {
        auto dataOrErr = getData();
        if (dataOrErr) {
          patchedData.assign(dataOrErr->begin(), dataOrErr->end());
        }
      }
    }
  }

public:
  /// Garbage collection support
  bool isLive() const { return live; }
  void markLive() { live = true; }
  void markDead() { live = false; }

private:
  InputFile *inputFile;
  unsigned sectionIndex;
  llvm::PEF::SectionHeader header;
  uint64_t virtualAddress = 0;
  bool live = false;  // For garbage collection

  // ELF-specific data
  bool fromELF;
  StringRef elfName;
  uint8_t elfKind = 0;
  std::vector<uint8_t> elfData;
  uint32_t elfAlignment = 1;

  // Relocation instructions from input file (16-bit PEF opcodes)
  SmallVector<uint16_t, 0> relocInstructions;

  // ELF relocations (for ELF input files)
  SmallVector<InputSectionReloc, 0> elfRelocations;

  // Patched data (after relocation processing)
  std::vector<uint8_t> patchedData;
};

} // namespace lld::pef

#endif
