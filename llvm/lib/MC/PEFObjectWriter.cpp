//===-- lib/MC/PEFObjectWriter.cpp - PEF File Writer ---------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements PEF (Preferred Executable Format) object file writer
// for Classic Mac OS targets.
//
//===----------------------------------------------------------------------===//

#include "llvm/MC/MCPEFObjectWriter.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/BinaryFormat/PEF.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAssembler.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCFixup.h"
#include "llvm/MC/MCFragment.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCSection.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/MC/MCValue.h"
#include "llvm/Support/Alignment.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/EndianStream.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <cstring>
#include <map>

using namespace llvm;
using namespace llvm::support;

#define DEBUG_TYPE "pef-writer"

namespace {

// PEF relocation entry
struct PEFRelocation {
  uint64_t Offset;          // Offset within section
  const MCSymbol *Symbol;   // Target symbol
  uint16_t Type;            // Relocation type
  uint16_t Flags;           // Relocation flags
  int64_t Addend;           // Addend value

  PEFRelocation(uint64_t Offset, const MCSymbol *Symbol, uint16_t Type,
                uint16_t Flags, int64_t Addend = 0)
      : Offset(Offset), Symbol(Symbol), Type(Type), Flags(Flags),
        Addend(Addend) {}
};

// PEF section entry
struct PEFSectionEntry {
  StringRef Name;
  const MCSection *Section;
  uint32_t NameOffset;      // Offset in loader string table
  uint32_t DefaultAddress;  // Default load address
  uint32_t TotalLength;     // Total size of section
  uint32_t UnpackedLength;  // Size after unpacking
  uint32_t ContainerLength; // Size in container
  uint32_t ContainerOffset; // Offset in container
  uint8_t SectionKind;      // Section type
  uint8_t ShareKind;        // Sharing type
  uint8_t Alignment;        // Alignment (power of 2)
  uint8_t Reserved;

  SmallVector<char, 0> Data;
  SmallVector<PEFRelocation, 0> Relocations;

  PEFSectionEntry(StringRef Name, const MCSection *Section)
      : Name(Name), Section(Section), NameOffset(0), DefaultAddress(0),
        TotalLength(0), UnpackedLength(0), ContainerLength(0),
        ContainerOffset(0), SectionKind(PEF::kCodeSection),
        ShareKind(PEF::kContextShare), Alignment(4), Reserved(0) {}
};

// PEF symbol entry
struct PEFSymbolEntry {
  StringRef Name;
  const MCSymbol *Symbol;
  uint32_t NameOffset;
  uint32_t Value;
  int16_t SectionIndex;
  uint32_t SymbolClass;
  bool IsExported;

  PEFSymbolEntry(StringRef Name, const MCSymbol *Symbol, uint32_t Value,
                 int16_t SectionIndex, bool IsExported = true)
      : Name(Name), Symbol(Symbol), NameOffset(0), Value(Value),
        SectionIndex(SectionIndex), SymbolClass(PEF::kCodeSymbol),
        IsExported(IsExported) {}
};

class PEFWriter {
  raw_pwrite_stream &OS;
  bool IsLittleEndian;
  MCPEFObjectTargetWriter &TargetWriter;

  std::vector<PEFSectionEntry> Sections;
  std::vector<PEFSymbolEntry> ExportedSymbols;
  std::vector<PEFSymbolEntry> ImportedSymbols;
  DenseMap<const MCSymbol *, uint32_t> SymbolIndexMap;

  // String table for symbol and section names
  SmallString<256> StringTable;
  DenseMap<StringRef, uint32_t> StringTableMap;

  uint32_t FileOffset;

public:
  PEFWriter(raw_pwrite_stream &OS, bool IsLittleEndian,
            MCPEFObjectTargetWriter &TargetWriter)
      : OS(OS), IsLittleEndian(IsLittleEndian), TargetWriter(TargetWriter),
        FileOffset(0) {}

  void writeObject(MCAssembler &Asm);

private:
  void collectSections(MCAssembler &Asm);
  void collectSymbols(MCAssembler &Asm);
  void layoutSections();

  uint32_t addString(StringRef Str);

  void writeContainerHeader();
  void writeSectionHeaders();
  void writeSectionData();
  void writeLoaderSection();

  void write8(uint8_t Value);
  void write16(uint16_t Value);
  void write32(uint32_t Value);
  void writeBytes(ArrayRef<uint8_t> Data);
  void writeZeros(uint64_t NumBytes);
  void alignTo(uint32_t Alignment);
};

void PEFWriter::write8(uint8_t Value) {
  OS << char(Value);
  ++FileOffset;
}

void PEFWriter::write16(uint16_t Value) {
  if (IsLittleEndian)
    support::endian::write<uint16_t>(OS, Value, llvm::endianness::little);
  else
    support::endian::write<uint16_t>(OS, Value, llvm::endianness::big);
  FileOffset += 2;
}

void PEFWriter::write32(uint32_t Value) {
  if (IsLittleEndian)
    support::endian::write<uint32_t>(OS, Value, llvm::endianness::little);
  else
    support::endian::write<uint32_t>(OS, Value, llvm::endianness::big);
  FileOffset += 4;
}

void PEFWriter::writeBytes(ArrayRef<uint8_t> Data) {
  OS.write(reinterpret_cast<const char *>(Data.data()), Data.size());
  FileOffset += Data.size();
}

void PEFWriter::writeZeros(uint64_t NumBytes) {
  for (uint64_t i = 0; i < NumBytes; ++i)
    write8(0);
}

void PEFWriter::alignTo(uint32_t Alignment) {
  uint32_t Offset = FileOffset % Alignment;
  if (Offset != 0)
    writeZeros(Alignment - Offset);
}

uint32_t PEFWriter::addString(StringRef Str) {
  auto It = StringTableMap.find(Str);
  if (It != StringTableMap.end())
    return It->second;

  uint32_t Offset = StringTable.size();
  StringTableMap[Str] = Offset;
  StringTable.append(Str.begin(), Str.end());
  StringTable.push_back('\0');
  return Offset;
}

void PEFWriter::collectSections(MCAssembler &Asm) {
  for (MCSection &Sec : Asm) {
    if (Sec.getFragmentList().empty())
      continue;

    StringRef Name = Sec.getName();
    PEFSectionEntry Entry(Name, &Sec);

    // Determine section kind based on name
    if (Name.starts_with(".text") || Name.starts_with("__text")) {
      Entry.SectionKind = PEF::kCodeSection;
      Entry.SymbolClass = PEF::kCodeSymbol;
    } else if (Name.starts_with(".data") || Name.starts_with("__data")) {
      Entry.SectionKind = PEF::kUnpackedDataSection;
      Entry.SymbolClass = PEF::kDataSymbol;
    } else if (Name.starts_with(".bss") || Name.starts_with("__bss")) {
      Entry.SectionKind = PEF::kUnpackedDataSection;
      Entry.SymbolClass = PEF::kBSSSymbol;
    } else if (Name.starts_with(".rodata") || Name.starts_with("__rodata") ||
               Name.starts_with("__const")) {
      Entry.SectionKind = PEF::kUnpackedDataSection;
      Entry.SymbolClass = PEF::kDataSymbol;
    } else {
      Entry.SectionKind = PEF::kUnpackedDataSection;
      Entry.SymbolClass = PEF::kDataSymbol;
    }

    // Get section alignment
    Entry.Alignment = Log2(Sec.getAlign());

    // Collect section data
    for (const MCFragment &F : Sec) {
      SmallString<256> Code;
      raw_svector_ostream VecOS(Code);
      Asm.writeSectionData(VecOS, &Sec, nullptr);
      Entry.Data.append(Code.begin(), Code.end());
      break; // writeSectionData writes all fragments
    }

    Entry.UnpackedLength = Entry.Data.size();
    Entry.TotalLength = Entry.UnpackedLength;
    Entry.ContainerLength = Entry.UnpackedLength;

    // Add section name to string table
    Entry.NameOffset = addString(Entry.Name);

    Sections.push_back(std::move(Entry));
  }
}

void PEFWriter::collectSymbols(MCAssembler &Asm) {
  uint32_t SymbolIndex = 0;

  for (const MCSymbol &Sym : Asm.symbols()) {
    // Skip temporary symbols
    if (Sym.isTemporary())
      continue;

    // Skip undefined symbols for now
    if (!Sym.isDefined())
      continue;

    const auto &Fragment = *Sym.getFragment();
    const auto &Section = *Fragment.getParent();

    // Find section index
    int16_t SectionIndex = -1;
    for (size_t i = 0; i < Sections.size(); ++i) {
      if (Sections[i].Section == &Section) {
        SectionIndex = i;
        break;
      }
    }

    if (SectionIndex == -1)
      continue;

    uint64_t Address = Asm.getSymbolOffset(Sym);
    bool IsExported = !Sym.isPrivate();

    PEFSymbolEntry Entry(Sym.getName(), &Sym, Address, SectionIndex,
                         IsExported);
    Entry.NameOffset = addString(Entry.Name);
    Entry.SymbolClass = Sections[SectionIndex].SymbolClass;

    if (IsExported)
      ExportedSymbols.push_back(Entry);

    SymbolIndexMap[&Sym] = SymbolIndex++;
  }

  // Sort exported symbols by name for efficient lookup
  std::sort(ExportedSymbols.begin(), ExportedSymbols.end(),
            [](const PEFSymbolEntry &A, const PEFSymbolEntry &B) {
              return A.Name < B.Name;
            });
}

void PEFWriter::layoutSections() {
  // Container header is 40 bytes
  FileOffset = 40;

  // Section headers: 28 bytes each
  FileOffset += Sections.size() * 28;

  // Align to 16 bytes
  FileOffset = alignTo(16);

  // Assign offsets to sections
  for (auto &Section : Sections) {
    alignTo(1u << Section.Alignment);
    Section.ContainerOffset = FileOffset;
    FileOffset += Section.ContainerLength;
  }

  // Loader section will be added last
}

void PEFWriter::writeContainerHeader() {
  // Magic numbers: 'Joy!' and 'peff'
  write32(PEF::kPEFTag1);    // 'Joy!' = 0x4A6F7921
  write32(PEF::kPEFTag2);    // 'peff' = 0x70656666

  // Architecture type (e.g., 'pwpc' for PowerPC)
  write32(TargetWriter.getArchType());

  // Format version
  write32(PEF::kPEFVersion); // Version 1

  // Timestamp (0 for object files)
  write32(0);

  // Old definition version, old implementation version
  write32(0);
  write32(0);

  // Number of sections (including loader section)
  write32(Sections.size() + 1); // +1 for loader section

  // Number of instantiated sections (all but loader)
  write32(Sections.size());

  // Reserved
  write32(0);
}

void PEFWriter::writeSectionHeaders() {
  // Write headers for regular sections
  for (const auto &Section : Sections) {
    write32(Section.NameOffset);
    write32(Section.DefaultAddress);
    write32(Section.TotalLength);
    write32(Section.UnpackedLength);
    write32(Section.ContainerLength);
    write32(Section.ContainerOffset);

    write8(Section.SectionKind);
    write8(Section.ShareKind);
    write8(Section.Alignment);
    write8(Section.Reserved);
  }

  // Write loader section header
  // The loader section will be written after all other sections
  uint32_t LoaderOffset = FileOffset;

  // We'll update these after writing the loader section
  write32(addString("loader")); // Name offset
  write32(0);                   // Default address
  write32(0);                   // Total length (will be updated)
  write32(0);                   // Unpacked length (will be updated)
  write32(0);                   // Container length (will be updated)
  write32(LoaderOffset);        // Container offset
  write8(PEF::kLoaderSection);  // Section kind
  write8(PEF::kGlobalShare);    // Share kind
  write8(4);                    // Alignment (16 bytes)
  write8(0);                    // Reserved
}

void PEFWriter::writeSectionData() {
  for (const auto &Section : Sections) {
    alignTo(1u << Section.Alignment);
    writeBytes(ArrayRef<uint8_t>(
        reinterpret_cast<const uint8_t *>(Section.Data.data()),
        Section.Data.size()));
  }
}

void PEFWriter::writeLoaderSection() {
  uint64_t LoaderSectionStart = FileOffset;

  // Loader info header (56 bytes)
  write32(0);  // Main section (-1 if none)
  write32(0);  // Main offset
  write32(-1); // Init section (-1 if none)
  write32(0);  // Init offset
  write32(-1); // Term section (-1 if none)
  write32(0);  // Term offset

  // Number of imported libraries
  write32(0);

  // Total imported symbol count
  write32(ImportedSymbols.size());

  // Number of relocation sections
  write32(0);

  // Relocation instructions offset
  write32(56); // Right after header

  // Loader string table offset (after symbols)
  uint32_t StringTableOffset = 56 + (ExportedSymbols.size() * 10);
  write32(StringTableOffset);

  // Hash slot table offset
  write32(StringTableOffset);

  // Hash slot count (simple: no hash table for now)
  write32(0);

  // Exported symbol count
  write32(ExportedSymbols.size());

  // Write exported symbols
  for (const auto &Sym : ExportedSymbols) {
    write32(Sym.SymbolClass);
    write32(Sym.Value);
    write16(Sym.SectionIndex);
    write32(Sym.NameOffset);
  }

  // Write string table
  writeBytes(ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(StringTable.data()),
      StringTable.size()));

  // Align to 4 bytes
  alignTo(4);

  uint64_t LoaderSectionEnd = FileOffset;
  uint32_t LoaderSize = LoaderSectionEnd - LoaderSectionStart;

  // Update loader section header
  uint64_t LoaderHeaderOffset = 40 + (Sections.size() * 28);
  OS.pwrite(reinterpret_cast<const char *>(&LoaderSize), 4,
            LoaderHeaderOffset + 8);  // Total length
  OS.pwrite(reinterpret_cast<const char *>(&LoaderSize), 4,
            LoaderHeaderOffset + 12); // Unpacked length
  OS.pwrite(reinterpret_cast<const char *>(&LoaderSize), 4,
            LoaderHeaderOffset + 16); // Container length
}

void PEFWriter::writeObject(MCAssembler &Asm) {
  // Collect all sections and symbols
  collectSections(Asm);
  collectSymbols(Asm);

  // Layout sections
  layoutSections();

  // Write PEF container header
  writeContainerHeader();

  // Write section headers
  writeSectionHeaders();

  // Align to section data
  alignTo(16);

  // Write section data
  writeSectionData();

  // Write loader section
  writeLoaderSection();
}

} // end anonymous namespace

// MCPEFObjectTargetWriter implementation
MCPEFObjectTargetWriter::MCPEFObjectTargetWriter(uint32_t ArchType)
    : MCObjectTargetWriter(), ArchType(ArchType) {}

MCPEFObjectTargetWriter::~MCPEFObjectTargetWriter() = default;

// PEFObjectWriter implementation
PEFObjectWriter::PEFObjectWriter(std::unique_ptr<MCPEFObjectTargetWriter> MOTW,
                                 raw_pwrite_stream &OS, bool IsLittleEndian)
    : TargetObjectWriter(std::move(MOTW)), OS(OS),
      IsLittleEndian(IsLittleEndian) {}

PEFObjectWriter::~PEFObjectWriter() = default;

void PEFObjectWriter::reset() {}

void PEFObjectWriter::executePostLayoutBinding(MCAssembler &Asm) {}

void PEFObjectWriter::recordRelocation(MCAssembler &Asm,
                                       const MCFragment *Fragment,
                                       const MCFixup &Fixup, MCValue Target,
                                       uint64_t &FixedValue) {
  // TODO: Implement relocation recording
  // For now, we'll apply relocations directly in writeObject
}

bool PEFObjectWriter::isSymbolRefDifferenceFullyResolvedImpl(
    const MCAssembler &Asm, const MCSymbol &SymA, const MCFragment &FB,
    bool InSet, bool IsPCRel) const {
  // Conservative: assume symbols in different sections need relocations
  if (SymA.isUndefined())
    return false;

  const MCSection &SecA = SymA.getFragment()->getParent()->getSection();
  const MCSection &SecB = FB.getParent()->getSection();

  if (&SecA != &SecB)
    return false;

  return true;
}

uint64_t PEFObjectWriter::writeObject(MCAssembler &Asm) {
  auto &Writer =
      static_cast<MCPEFObjectTargetWriter &>(*this->TargetObjectWriter);
  PEFWriter W(OS, IsLittleEndian, Writer);
  W.writeObject(Asm);
  return 0;
}

std::unique_ptr<MCObjectWriter>
llvm::createPEFObjectWriter(std::unique_ptr<MCPEFObjectTargetWriter> MOTW,
                            raw_pwrite_stream &OS, bool IsLittleEndian) {
  return std::make_unique<PEFObjectWriter>(std::move(MOTW), OS,
                                           IsLittleEndian);
}
