//===- PEFObjectFile.h - PEF object file implementation ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares the PEFObjectFile class, which implements the ObjectFile
// interface for PEF (Preferred Executable Format) files used by Mac OS Classic
// PowerPC executables.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_OBJECT_PEFOBJECTFILE_H
#define LLVM_OBJECT_PEFOBJECTFILE_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/BinaryFormat/PEF.h"
#include "llvm/Object/Binary.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"
#include <cstdint>

namespace llvm {
namespace object {

/// PEFObjectFile - This class implements the ObjectFile interface for PEF
/// (Preferred Executable Format) files used by Mac OS Classic on PowerPC.
class PEFObjectFile : public ObjectFile {
private:
  // Cached container header
  PEF::ContainerHeader Header;

  // Cached section headers
  SmallVector<PEF::SectionHeader, 4> SectionHeaders;

  // Loader section data (if present)
  const uint8_t *LoaderSectionData = nullptr;
  uint64_t LoaderSectionSize = 0;

  // String table offset within loader section
  uint64_t LoaderStringsOffset = 0;

  PEFObjectFile(MemoryBufferRef Object, Error &Err);

  /// Parse and validate the PEF container header.
  Error parseHeader();

  /// Parse section headers.
  Error parseSectionHeaders();

  /// Find and cache the loader section.
  Error parseLoaderSection();

public:
  static Expected<std::unique_ptr<PEFObjectFile>>
  create(MemoryBufferRef Object);

  /// Get the container header.
  const PEF::ContainerHeader &getHeader() const { return Header; }

  /// Get a section header by index.
  Expected<PEF::SectionHeader> getSectionHeader(unsigned Index) const;

  /// Get the number of sections.
  unsigned getSectionCount() const { return Header.SectionCount; }

  /// Get section data.
  Expected<ArrayRef<uint8_t>> getSectionData(unsigned SectionIndex) const;

  /// Get the loader info header (if loader section exists).
  Expected<PEF::LoaderInfoHeader> getLoaderInfoHeader() const;

  /// Get a string from the loader string table.
  Expected<StringRef> getLoaderString(uint32_t Offset) const;

  /// Phase 3: Get relocation header at offset within loader section
  Expected<PEF::LoaderRelocationHeader> getRelocHeader(uint64_t Offset) const;

  /// Phase 3: Get relocation instructions
  Expected<ArrayRef<uint16_t>> getRelocInstructions(uint64_t Offset,
                                                     uint32_t Count) const;

  /// Phase 3: Get imported symbol name by index
  Expected<StringRef> getImportedSymbolName(uint32_t Index) const;

  // ObjectFile interface implementation
  void moveSymbolNext(DataRefImpl &Symb) const override;
  Expected<StringRef> getSymbolName(DataRefImpl Symb) const override;
  Expected<uint64_t> getSymbolAddress(DataRefImpl Symb) const override;
  uint64_t getSymbolValueImpl(DataRefImpl Symb) const override;
  uint32_t getSymbolAlignment(DataRefImpl Symb) const override;
  uint64_t getCommonSymbolSizeImpl(DataRefImpl Symb) const override;
  Expected<SymbolRef::Type> getSymbolType(DataRefImpl Symb) const override;
  Expected<section_iterator> getSymbolSection(DataRefImpl Symb) const override;
  Expected<uint32_t> getSymbolFlags(DataRefImpl Symb) const override;

  void moveSectionNext(DataRefImpl &Sec) const override;
  Expected<StringRef> getSectionName(DataRefImpl Sec) const override;
  uint64_t getSectionAddress(DataRefImpl Sec) const override;
  uint64_t getSectionIndex(DataRefImpl Sec) const override;
  uint64_t getSectionSize(DataRefImpl Sec) const override;
  Expected<ArrayRef<uint8_t>> getSectionContents(DataRefImpl Sec) const override;
  uint64_t getSectionAlignment(DataRefImpl Sec) const override;
  bool isSectionCompressed(DataRefImpl Sec) const override;
  bool isSectionText(DataRefImpl Sec) const override;
  bool isSectionData(DataRefImpl Sec) const override;
  bool isSectionBSS(DataRefImpl Sec) const override;
  bool isSectionVirtual(DataRefImpl Sec) const override;
  relocation_iterator section_rel_begin(DataRefImpl Sec) const override;
  relocation_iterator section_rel_end(DataRefImpl Sec) const override;

  void moveRelocationNext(DataRefImpl &Rel) const override;
  uint64_t getRelocationOffset(DataRefImpl Rel) const override;
  symbol_iterator getRelocationSymbol(DataRefImpl Rel) const override;
  uint64_t getRelocationType(DataRefImpl Rel) const override;
  void getRelocationTypeName(DataRefImpl Rel,
                              SmallVectorImpl<char> &Result) const override;

  section_iterator section_begin() const override;
  section_iterator section_end() const override;

  basic_symbol_iterator symbol_begin() const override;
  basic_symbol_iterator symbol_end() const override;

  uint8_t getBytesInAddress() const override;
  StringRef getFileFormatName() const override;
  Triple::ArchType getArch() const override;
  Expected<SubtargetFeatures> getFeatures() const override;
  Expected<uint64_t> getStartAddress() const override;

  bool isRelocatableObject() const override { return false; }

  bool is64Bit() const override { return false; } // PEF is 32-bit only

  /// Static methods for type checking
  static bool classof(const Binary *V) {
    return V->isPEF();
  }
};

/// Iterator for exported symbols in the loader section
class PEFExportIterator {
  const PEFObjectFile *Obj;
  unsigned Index;

public:
  PEFExportIterator(const PEFObjectFile *Obj, unsigned Index)
      : Obj(Obj), Index(Index) {}

  bool operator==(const PEFExportIterator &Other) const {
    return Index == Other.Index;
  }

  bool operator!=(const PEFExportIterator &Other) const {
    return !(*this == Other);
  }

  PEFExportIterator &operator++() {
    ++Index;
    return *this;
  }

  /// Get the current export symbol.
  Expected<PEF::ExportedSymbol> operator*() const;

  /// Get the export symbol name.
  Expected<StringRef> getName() const;

  /// Get the export symbol value (offset in section).
  uint32_t getValue() const;

  /// Get the section index.
  int16_t getSectionIndex() const;

  /// Get the symbol class.
  uint8_t getSymbolClass() const;
};

/// Iterator for imported libraries in the loader section
class PEFImportLibIterator {
  const PEFObjectFile *Obj;
  unsigned Index;

public:
  PEFImportLibIterator(const PEFObjectFile *Obj, unsigned Index)
      : Obj(Obj), Index(Index) {}

  bool operator==(const PEFImportLibIterator &Other) const {
    return Index == Other.Index;
  }

  bool operator!=(const PEFImportLibIterator &Other) const {
    return !(*this == Other);
  }

  PEFImportLibIterator &operator++() {
    ++Index;
    return *this;
  }

  /// Get the current imported library.
  Expected<PEF::ImportedLibrary> operator*() const;

  /// Get the library name.
  Expected<StringRef> getName() const;

  /// Get the number of symbols imported from this library.
  uint32_t getSymbolCount() const;
};

/// Helper functions for byte-swapping PEF structures (big-endian format)
namespace PEFSupport {
  inline uint16_t read16be(const uint8_t *P) {
    return support::endian::read16be(P);
  }

  inline uint32_t read32be(const uint8_t *P) {
    return support::endian::read32be(P);
  }

  inline int16_t read16sbe(const uint8_t *P) {
    return static_cast<int16_t>(support::endian::read16be(P));
  }

  inline int32_t read32sbe(const uint8_t *P) {
    return static_cast<int32_t>(support::endian::read32be(P));
  }

  /// Read and byte-swap a ContainerHeader
  PEF::ContainerHeader readContainerHeader(const uint8_t *Data);

  /// Read and byte-swap a SectionHeader
  PEF::SectionHeader readSectionHeader(const uint8_t *Data);

  /// Read and byte-swap a LoaderInfoHeader
  PEF::LoaderInfoHeader readLoaderInfoHeader(const uint8_t *Data);

  /// Read and byte-swap an ImportedLibrary
  PEF::ImportedLibrary readImportedLibrary(const uint8_t *Data);

  /// Read and byte-swap an ExportedSymbol
  PEF::ExportedSymbol readExportedSymbol(const uint8_t *Data);
} // end namespace PEFSupport

} // end namespace object
} // end namespace llvm

#endif // LLVM_OBJECT_PEFOBJECTFILE_H
