//===- PEFObjectFile.cpp - PEF object file implementation -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the PEFObjectFile class, which implements reading of
// PEF (Preferred Executable Format) files used by Mac OS Classic PowerPC.
//
//===----------------------------------------------------------------------===//

#include "llvm/Object/PEFObjectFile.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/PEF.h"
#include "llvm/Object/Error.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/TargetParser/SubtargetFeature.h"

using namespace llvm;
using namespace llvm::object;
using namespace llvm::PEF;

//===----------------------------------------------------------------------===//
// PEFSupport - Helper functions for reading big-endian PEF structures
//===----------------------------------------------------------------------===//

ContainerHeader PEFSupport::readContainerHeader(const uint8_t *Data) {
  ContainerHeader H;
  H.Tag1 = read32be(Data + 0);
  H.Tag2 = read32be(Data + 4);
  H.Architecture = read32be(Data + 8);
  H.FormatVersion = read32be(Data + 12);
  H.DateTimeStamp = read32be(Data + 16);
  H.OldDefVersion = read32be(Data + 20);
  H.OldImpVersion = read32be(Data + 24);
  H.CurrentVersion = read32be(Data + 28);
  H.SectionCount = read16be(Data + 32);
  H.InstSectionCount = read16be(Data + 34);
  H.ReservedA = read32be(Data + 36);
  return H;
}

SectionHeader PEFSupport::readSectionHeader(const uint8_t *Data) {
  SectionHeader H;
  H.NameOffset = read32sbe(Data + 0);
  H.DefaultAddress = read32be(Data + 4);
  H.TotalLength = read32be(Data + 8);
  H.UnpackedLength = read32be(Data + 12);
  H.ContainerLength = read32be(Data + 16);
  H.ContainerOffset = read32be(Data + 20);
  H.SectionKind = Data[24];
  H.ShareKind = Data[25];
  H.Alignment = Data[26];
  H.ReservedA = Data[27];
  return H;
}

LoaderInfoHeader PEFSupport::readLoaderInfoHeader(const uint8_t *Data) {
  LoaderInfoHeader H;
  H.MainSection = read32sbe(Data + 0);
  H.MainOffset = read32be(Data + 4);
  H.InitSection = read32sbe(Data + 8);
  H.InitOffset = read32be(Data + 12);
  H.TermSection = read32sbe(Data + 16);
  H.TermOffset = read32be(Data + 20);
  H.ImportedLibraryCount = read32be(Data + 24);
  H.TotalImportedSymbolCount = read32be(Data + 28);
  H.RelocSectionCount = read32be(Data + 32);
  H.RelocInstrOffset = read32be(Data + 36);
  H.LoaderStringsOffset = read32be(Data + 40);
  H.ExportHashOffset = read32be(Data + 44);
  H.ExportHashTablePower = read32be(Data + 48);
  H.ExportedSymbolCount = read32be(Data + 52);
  return H;
}

ImportedLibrary PEFSupport::readImportedLibrary(const uint8_t *Data) {
  ImportedLibrary L;
  L.NameOffset = read32be(Data + 0);
  L.OldImpVersion = read32be(Data + 4);
  L.CurrentVersion = read32be(Data + 8);
  L.ImportedSymbolCount = read32be(Data + 12);
  L.FirstImportedSymbol = read32be(Data + 16);
  L.Options = Data[20];
  L.ReservedA = Data[21];
  L.ReservedB = read16be(Data + 22);
  return L;
}

ExportedSymbol PEFSupport::readExportedSymbol(const uint8_t *Data) {
  ExportedSymbol S;
  S.ClassAndName = read32be(Data + 0);
  S.SymbolValue = read32be(Data + 4);
  S.SectionIndex = read16sbe(Data + 8);
  return S;
}

//===----------------------------------------------------------------------===//
// PEFObjectFile implementation
//===----------------------------------------------------------------------===//

PEFObjectFile::PEFObjectFile(MemoryBufferRef Object, Error &Err)
    : ObjectFile(Binary::ID_PEF, Object) {
  ErrorAsOutParameter ErrAsOutParam(&Err);

  // Parse header
  if (Error E = parseHeader()) {
    Err = std::move(E);
    return;
  }

  // Parse section headers
  if (Error E = parseSectionHeaders()) {
    Err = std::move(E);
    return;
  }

  // Parse loader section if present
  if (Error E = parseLoaderSection()) {
    Err = std::move(E);
    return;
  }
}

Expected<std::unique_ptr<PEFObjectFile>>
PEFObjectFile::create(MemoryBufferRef Object) {
  Error Err = Error::success();
  std::unique_ptr<PEFObjectFile> Ret(new PEFObjectFile(Object, Err));
  if (Err)
    return std::move(Err);
  return std::move(Ret);
}

Expected<std::unique_ptr<ObjectFile>>
ObjectFile::createPEFObjectFile(MemoryBufferRef Object) {
  return PEFObjectFile::create(Object);
}

Error PEFObjectFile::parseHeader() {
  StringRef Data = getData();

  // Check minimum size for container header
  if (Data.size() < sizeof(ContainerHeader))
    return createError("file too small for PEF container header");

  // Read and validate container header
  Header = PEFSupport::readContainerHeader(
      reinterpret_cast<const uint8_t *>(Data.data()));

  // Validate magic numbers
  if (Header.Tag1 != kPEFTag1 || Header.Tag2 != kPEFTag2)
    return createError("invalid PEF magic numbers");

  // Validate format version
  if (Header.FormatVersion != kPEFVersion)
    return createError("unsupported PEF format version");

  // Validate architecture (PowerPC or 68K)
  if (Header.Architecture != kPEFPowerPCArch &&
      Header.Architecture != kPEFM68KArch)
    return createError("unsupported PEF architecture");

  // Validate section count
  if (Header.SectionCount == 0)
    return createError("PEF container has no sections");

  return Error::success();
}

Error PEFObjectFile::parseSectionHeaders() {
  StringRef Data = getData();

  // Calculate required size
  uint64_t SectionHeadersSize =
      static_cast<uint64_t>(Header.SectionCount) * sizeof(SectionHeader);
  uint64_t RequiredSize = sizeof(ContainerHeader) + SectionHeadersSize;

  if (Data.size() < RequiredSize)
    return createError("file too small for section headers");

  // Read all section headers
  const uint8_t *SectionData =
      reinterpret_cast<const uint8_t *>(Data.data()) + sizeof(ContainerHeader);

  SectionHeaders.reserve(Header.SectionCount);
  for (unsigned I = 0; I < Header.SectionCount; ++I) {
    SectionHeader Hdr = PEFSupport::readSectionHeader(
        SectionData + I * sizeof(SectionHeader));

    // Validate section offset and size
    if (Hdr.ContainerLength > 0) {
      uint64_t SectionEnd =
          static_cast<uint64_t>(Hdr.ContainerOffset) + Hdr.ContainerLength;
      if (SectionEnd > Data.size())
        return createError("section extends past end of file");
    }

    SectionHeaders.push_back(Hdr);
  }

  return Error::success();
}

Error PEFObjectFile::parseLoaderSection() {
  // Find loader section
  for (unsigned I = 0; I < Header.SectionCount; ++I) {
    const SectionHeader &Hdr = SectionHeaders[I];
    if (Hdr.SectionKind == kPEFLoaderSection) {
      if (Hdr.ContainerLength == 0)
        return createError("loader section has zero length");

      StringRef Data = getData();
      LoaderSectionData = reinterpret_cast<const uint8_t *>(
          Data.data() + Hdr.ContainerOffset);
      LoaderSectionSize = Hdr.ContainerLength;

      // Validate loader info header fits
      if (LoaderSectionSize < sizeof(LoaderInfoHeader))
        return createError("loader section too small for header");

      // Read loader info header to get string table offset
      LoaderInfoHeader LoaderInfo =
          PEFSupport::readLoaderInfoHeader(LoaderSectionData);
      LoaderStringsOffset = LoaderInfo.LoaderStringsOffset;

      break;
    }
  }

  return Error::success();
}

Expected<SectionHeader>
PEFObjectFile::getSectionHeader(unsigned Index) const {
  if (Index >= Header.SectionCount)
    return createError("section index out of range");
  return SectionHeaders[Index];
}

Expected<ArrayRef<uint8_t>>
PEFObjectFile::getSectionData(unsigned SectionIndex) const {
  if (SectionIndex >= Header.SectionCount)
    return createError("section index out of range");

  const SectionHeader &Hdr = SectionHeaders[SectionIndex];

  // Empty sections return empty array
  if (Hdr.ContainerLength == 0)
    return ArrayRef<uint8_t>();

  StringRef Data = getData();
  const uint8_t *Start =
      reinterpret_cast<const uint8_t *>(Data.data()) + Hdr.ContainerOffset;

  return ArrayRef<uint8_t>(Start, Hdr.ContainerLength);
}

Expected<LoaderInfoHeader> PEFObjectFile::getLoaderInfoHeader() const {
  if (!LoaderSectionData)
    return createError("no loader section in container");

  if (LoaderSectionSize < sizeof(LoaderInfoHeader))
    return createError("loader section too small");

  return PEFSupport::readLoaderInfoHeader(LoaderSectionData);
}

Expected<StringRef> PEFObjectFile::getLoaderString(uint32_t Offset) const {
  if (!LoaderSectionData)
    return createError("no loader section in container");

  if (Offset >= LoaderSectionSize)
    return createError("string offset out of range");

  const char *StrStart =
      reinterpret_cast<const char *>(LoaderSectionData + Offset);
  const char *StrEnd =
      reinterpret_cast<const char *>(LoaderSectionData + LoaderSectionSize);

  // Find null terminator
  const char *End = static_cast<const char *>(
      memchr(StrStart, '\0', StrEnd - StrStart));

  if (!End)
    return createError("string not null-terminated");

  return StringRef(StrStart, End - StrStart);
}

//===----------------------------------------------------------------------===//
// ObjectFile interface implementation
//===----------------------------------------------------------------------===//

void PEFObjectFile::moveSymbolNext(DataRefImpl &Symb) const {
  Symb.d.a++;
}

/// Calculate the offset to the exported symbol table within the loader section.
/// According to the PEF specification, the loader section layout is:
/// 1. LoaderInfoHeader (56 bytes)
/// 2. ImportedLibrary array
/// 3. ImportedSymbol array (4 bytes each)
/// 4. RelocHeader array (12 bytes each)
/// 5. Relocation instructions
/// 6. Loader string table
/// 7. Export hash table + Export key table
/// 8. Exported symbol table (10 bytes each) <- what we need
///
/// The ExportHashOffset field points to the start of the hash/key tables.
/// We calculate the exported symbol table offset by adding the hash and key table sizes.
static uint64_t getExportedSymbolTableOffset(const LoaderInfoHeader &LoaderInfo) {
  // Hash table size: 2^exportHashTablePower slots, 4 bytes per slot
  uint32_t HashSlotCount = 1u << LoaderInfo.ExportHashTablePower;
  uint32_t HashTableSize = HashSlotCount * 4;

  // Key table size: one 4-byte entry per exported symbol
  uint32_t KeyTableSize = LoaderInfo.ExportedSymbolCount * 4;

  // Exported symbol table comes immediately after hash and key tables
  return LoaderInfo.ExportHashOffset + HashTableSize + KeyTableSize;
}

Expected<StringRef> PEFObjectFile::getSymbolName(DataRefImpl Symb) const {
  // PEF symbols are in the loader section's export table
  if (!LoaderSectionData)
    return createError("no loader section");

  Expected<LoaderInfoHeader> LoaderInfoOrErr = getLoaderInfoHeader();
  if (!LoaderInfoOrErr)
    return LoaderInfoOrErr.takeError();

  LoaderInfoHeader LoaderInfo = *LoaderInfoOrErr;
  if (Symb.d.a >= LoaderInfo.ExportedSymbolCount)
    return createError("symbol index out of range");

  // Calculate export symbol table offset using hash/key table sizes
  uint64_t ExportTableOffset = getExportedSymbolTableOffset(LoaderInfo);

  // Note: ExportedSymbol struct has padding, but on-disk format is exactly 10 bytes
  constexpr uint32_t KExportedSymbolSize = 10;
  const uint8_t *ExportData = LoaderSectionData + ExportTableOffset +
      Symb.d.a * KExportedSymbolSize;

  ExportedSymbol Sym = PEFSupport::readExportedSymbol(ExportData);
  uint32_t NameOffset = getExportedSymbolNameOffset(Sym.ClassAndName);

  // NameOffset is relative to the loader string table start
  return getLoaderString(LoaderStringsOffset + NameOffset);
}

Expected<uint64_t> PEFObjectFile::getSymbolAddress(DataRefImpl Symb) const {
  return getSymbolValueImpl(Symb);
}

uint64_t PEFObjectFile::getSymbolValueImpl(DataRefImpl Symb) const {
  // Return the symbol value (offset in section)
  if (!LoaderSectionData)
    return 0;

  Expected<LoaderInfoHeader> LoaderInfoOrErr = getLoaderInfoHeader();
  if (!LoaderInfoOrErr)
    return 0;

  LoaderInfoHeader LoaderInfo = *LoaderInfoOrErr;
  if (Symb.d.a >= LoaderInfo.ExportedSymbolCount)
    return 0;

  uint64_t ExportTableOffset = getExportedSymbolTableOffset(LoaderInfo);

  constexpr uint32_t KExportedSymbolSize = 10;
  const uint8_t *ExportData = LoaderSectionData + ExportTableOffset +
      Symb.d.a * KExportedSymbolSize;

  ExportedSymbol Sym = PEFSupport::readExportedSymbol(ExportData);
  return Sym.SymbolValue;
}

uint32_t PEFObjectFile::getSymbolAlignment(DataRefImpl Symb) const {
  return 0; // PEF doesn't specify symbol alignment
}

uint64_t PEFObjectFile::getCommonSymbolSizeImpl(DataRefImpl Symb) const {
  return 0; // PEF doesn't have common symbols
}

Expected<SymbolRef::Type>
PEFObjectFile::getSymbolType(DataRefImpl Symb) const {
  if (!LoaderSectionData)
    return SymbolRef::ST_Unknown;

  Expected<LoaderInfoHeader> LoaderInfoOrErr = getLoaderInfoHeader();
  if (!LoaderInfoOrErr)
    return LoaderInfoOrErr.takeError();

  LoaderInfoHeader LoaderInfo = *LoaderInfoOrErr;
  if (Symb.d.a >= LoaderInfo.ExportedSymbolCount)
    return SymbolRef::ST_Unknown;

  uint64_t ExportTableOffset = getExportedSymbolTableOffset(LoaderInfo);

  constexpr uint32_t KExportedSymbolSize = 10;
  const uint8_t *ExportData = LoaderSectionData + ExportTableOffset +
      Symb.d.a * KExportedSymbolSize;

  ExportedSymbol Sym = PEFSupport::readExportedSymbol(ExportData);
  uint8_t SymClass = getExportedSymbolClass(Sym.ClassAndName);

  switch (SymClass) {
  case kPEFCodeSymbol:
  case kPEFGlueSymbol:
    return SymbolRef::ST_Function;
  case kPEFDataSymbol:
  case kPEFTOCSymbol:
    return SymbolRef::ST_Data;
  case kPEFTVectorSymbol:
    return SymbolRef::ST_Function; // Transition vector is like a function
  default:
    return SymbolRef::ST_Unknown;
  }
}

Expected<section_iterator>
PEFObjectFile::getSymbolSection(DataRefImpl Symb) const {
  if (!LoaderSectionData) {
    DataRefImpl Sec;
    Sec.d.a = 0;
    return section_iterator(SectionRef(Sec, this));
  }

  Expected<LoaderInfoHeader> LoaderInfoOrErr = getLoaderInfoHeader();
  if (!LoaderInfoOrErr)
    return LoaderInfoOrErr.takeError();

  LoaderInfoHeader LoaderInfo = *LoaderInfoOrErr;
  if (Symb.d.a >= LoaderInfo.ExportedSymbolCount) {
    DataRefImpl Sec;
    Sec.d.a = 0;
    return section_iterator(SectionRef(Sec, this));
  }

  uint64_t ExportTableOffset = getExportedSymbolTableOffset(LoaderInfo);

  constexpr uint32_t KExportedSymbolSize = 10;
  const uint8_t *ExportData = LoaderSectionData + ExportTableOffset +
      Symb.d.a * KExportedSymbolSize;

  ExportedSymbol Sym = PEFSupport::readExportedSymbol(ExportData);

  DataRefImpl Sec;
  if (Sym.SectionIndex >= 0 && Sym.SectionIndex < Header.SectionCount)
    Sec.d.a = Sym.SectionIndex;
  else
    Sec.d.a = 0;

  return section_iterator(SectionRef(Sec, this));
}

Expected<uint32_t> PEFObjectFile::getSymbolFlags(DataRefImpl Symb) const {
  // PEF exported symbols are global and visible
  return SymbolRef::SF_Global | SymbolRef::SF_Exported;
}

void PEFObjectFile::moveSectionNext(DataRefImpl &Sec) const {
  Sec.d.a++;
}

Expected<StringRef> PEFObjectFile::getSectionName(DataRefImpl Sec) const {
  if (Sec.d.a >= Header.SectionCount)
    return createError("section index out of range");

  const SectionHeader &Hdr = SectionHeaders[Sec.d.a];

  // PEF sections can have names in the string table, or use default names
  if (Hdr.NameOffset >= 0 && LoaderSectionData) {
    if (auto NameOrErr = getLoaderString(LoaderStringsOffset + Hdr.NameOffset))
      return *NameOrErr;
  }

  // Return default name based on section kind
  switch (Hdr.SectionKind) {
  case kPEFCodeSection: return ".text";
  case kPEFUnpackedDataSection: return ".data";
  case kPEFPatternDataSection: return ".pattern";
  case kPEFConstantSection: return ".rodata";
  case kPEFLoaderSection: return ".loader";
  case kPEFDebugSection: return ".debug";
  case kPEFExecutableDataSection: return ".exdata";
  case kPEFExceptionSection: return ".except";
  case kPEFTracebackSection: return ".traceback";
  default: return ".unknown";
  }
}

uint64_t PEFObjectFile::getSectionAddress(DataRefImpl Sec) const {
  if (Sec.d.a >= Header.SectionCount)
    return 0;
  return SectionHeaders[Sec.d.a].DefaultAddress;
}

uint64_t PEFObjectFile::getSectionIndex(DataRefImpl Sec) const {
  return Sec.d.a;
}

uint64_t PEFObjectFile::getSectionSize(DataRefImpl Sec) const {
  if (Sec.d.a >= Header.SectionCount)
    return 0;
  return SectionHeaders[Sec.d.a].TotalLength;
}

Expected<ArrayRef<uint8_t>>
PEFObjectFile::getSectionContents(DataRefImpl Sec) const {
  return getSectionData(Sec.d.a);
}

uint64_t PEFObjectFile::getSectionAlignment(DataRefImpl Sec) const {
  if (Sec.d.a >= Header.SectionCount)
    return 0;
  // Alignment is power of 2
  return 1ULL << SectionHeaders[Sec.d.a].Alignment;
}

bool PEFObjectFile::isSectionCompressed(DataRefImpl Sec) const {
  if (Sec.d.a >= Header.SectionCount)
    return false;
  // Pattern-initialized data is a form of compression
  return SectionHeaders[Sec.d.a].SectionKind == kPEFPatternDataSection;
}

bool PEFObjectFile::isSectionText(DataRefImpl Sec) const {
  if (Sec.d.a >= Header.SectionCount)
    return false;
  uint8_t Kind = SectionHeaders[Sec.d.a].SectionKind;
  return Kind == kPEFCodeSection || Kind == kPEFExecutableDataSection;
}

bool PEFObjectFile::isSectionData(DataRefImpl Sec) const {
  if (Sec.d.a >= Header.SectionCount)
    return false;
  uint8_t Kind = SectionHeaders[Sec.d.a].SectionKind;
  return Kind == kPEFUnpackedDataSection ||
         Kind == kPEFPatternDataSection ||
         Kind == kPEFConstantSection;
}

bool PEFObjectFile::isSectionBSS(DataRefImpl Sec) const {
  if (Sec.d.a >= Header.SectionCount)
    return false;
  const SectionHeader &Hdr = SectionHeaders[Sec.d.a];
  // BSS is represented as unpacked data with zero container length
  return Hdr.SectionKind == kPEFUnpackedDataSection &&
         Hdr.UnpackedLength > Hdr.ContainerLength;
}

bool PEFObjectFile::isSectionVirtual(DataRefImpl Sec) const {
  return isSectionBSS(Sec);
}

relocation_iterator
PEFObjectFile::section_rel_begin(DataRefImpl Sec) const {
  DataRefImpl Rel;
  Rel.d.a = 0;
  Rel.d.b = Sec.d.a;
  return relocation_iterator(RelocationRef(Rel, this));
}

relocation_iterator
PEFObjectFile::section_rel_end(DataRefImpl Sec) const {
  DataRefImpl Rel;
  Rel.d.a = 0; // PEF relocations not yet implemented
  Rel.d.b = Sec.d.a;
  return relocation_iterator(RelocationRef(Rel, this));
}

void PEFObjectFile::moveRelocationNext(DataRefImpl &Rel) const {
  Rel.d.a++;
}

uint64_t PEFObjectFile::getRelocationOffset(DataRefImpl Rel) const {
  return 0; // TODO: Implement when we need relocation support
}

symbol_iterator PEFObjectFile::getRelocationSymbol(DataRefImpl Rel) const {
  DataRefImpl Sym;
  Sym.d.a = 0;
  return symbol_iterator(SymbolRef(Sym, this));
}

uint64_t PEFObjectFile::getRelocationType(DataRefImpl Rel) const {
  return 0; // TODO: Implement
}

void PEFObjectFile::getRelocationTypeName(
    DataRefImpl Rel, SmallVectorImpl<char> &Result) const {
  Result.clear();
  // TODO: Implement
}

section_iterator PEFObjectFile::section_begin() const {
  DataRefImpl Sec;
  Sec.d.a = 0;
  return section_iterator(SectionRef(Sec, this));
}

section_iterator PEFObjectFile::section_end() const {
  DataRefImpl Sec;
  Sec.d.a = Header.SectionCount;
  return section_iterator(SectionRef(Sec, this));
}

basic_symbol_iterator PEFObjectFile::symbol_begin() const {
  DataRefImpl Sym;
  Sym.d.a = 0;
  return basic_symbol_iterator(SymbolRef(Sym, this));
}

basic_symbol_iterator PEFObjectFile::symbol_end() const {
  DataRefImpl Sym;
  if (LoaderSectionData) {
    if (Expected<LoaderInfoHeader> LoaderInfo = getLoaderInfoHeader())
      Sym.d.a = LoaderInfo->ExportedSymbolCount;
    else
      Sym.d.a = 0;
  } else {
    Sym.d.a = 0;
  }
  return basic_symbol_iterator(SymbolRef(Sym, this));
}

uint8_t PEFObjectFile::getBytesInAddress() const {
  return 4; // PEF is 32-bit
}

StringRef PEFObjectFile::getFileFormatName() const {
  return "PEF";
}

Triple::ArchType PEFObjectFile::getArch() const {
  if (Header.Architecture == kPEFPowerPCArch)
    return Triple::ppc;
  if (Header.Architecture == kPEFM68KArch)
    return Triple::m68k;
  return Triple::UnknownArch;
}

Expected<SubtargetFeatures> PEFObjectFile::getFeatures() const {
  SubtargetFeatures Features;
  return Features;
}

Expected<uint64_t> PEFObjectFile::getStartAddress() const {
  if (!LoaderSectionData)
    return 0;

  Expected<LoaderInfoHeader> LoaderInfoOrErr = getLoaderInfoHeader();
  if (!LoaderInfoOrErr)
    return LoaderInfoOrErr.takeError();

  LoaderInfoHeader LoaderInfo = *LoaderInfoOrErr;

  // Return main offset in main section
  if (LoaderInfo.MainSection >= 0 &&
      LoaderInfo.MainSection < Header.SectionCount)
    return LoaderInfo.MainOffset;

  return 0;
}
