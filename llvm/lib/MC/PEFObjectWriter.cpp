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
#include "llvm/ADT/StringExtras.h"
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
        ContainerOffset(0), SectionKind(PEF::kPEFCodeSection),
        ShareKind(PEF::kPEFProcessShare), Alignment(4), Reserved(0) {}
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
        SectionIndex(SectionIndex), SymbolClass(PEF::kPEFCodeSymbol),
        IsExported(IsExported) {}
};

class PEFWriter {
  raw_pwrite_stream &OS;
  MCPEFObjectTargetWriter &TargetWriter;

  std::vector<PEFSectionEntry> Sections;
  std::vector<PEFSymbolEntry> ExportedSymbols;
  std::vector<PEFSymbolEntry> ImportedSymbols;
  DenseMap<const MCSymbol *, uint32_t> SymbolIndexMap;

  // Map from input section to its offset in the merged output section
  // Used to compute correct symbol offsets when multiple sections are merged
  DenseMap<const MCSection *, uint64_t> SectionMergeOffset;

  // String table for symbol and section names
  SmallString<256> StringTable;
  DenseMap<StringRef, uint32_t> StringTableMap;

  uint32_t FileOffset;

public:
  PEFWriter(raw_pwrite_stream &OS, MCPEFObjectTargetWriter &TargetWriter)
      : OS(OS), TargetWriter(TargetWriter), FileOffset(0) {}

  void writeObject(MCAssembler &Asm,
                   const std::vector<PEFObjectWriter::StoredRelocation> &Relocs);

private:
  void collectSections(MCAssembler &Asm,
                       const std::vector<PEFObjectWriter::StoredRelocation> &Relocs);
  void patchMergeOffsets(MCAssembler &Asm);
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
  // PEF is always big-endian (PowerPC)
  support::endian::write<uint16_t>(OS, Value, llvm::endianness::big);
  FileOffset += 2;
}

void PEFWriter::write32(uint32_t Value) {
  // PEF is always big-endian (PowerPC)
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

void PEFWriter::collectSections(MCAssembler &Asm,
                                const std::vector<PEFObjectWriter::StoredRelocation> &Relocs) {
  // PEF has only two main sections: code (.text) and data (.data)
  // We need to merge all code-like sections and all data-like sections
  // Use indices instead of pointers since the vector may reallocate
  int TextIdx = -1;
  int DataIdx = -1;

  for (MCSection &Sec : Asm) {
    // Skip empty sections
    if (Sec.begin() == Sec.end())
      continue;

    StringRef Name = Sec.getName();

    // Skip sections we don't care about
    if (Name.starts_with(".note") || Name.starts_with(".comment"))
      continue;

    // Determine if this is a code or data section
    bool IsCode = Name.starts_with(".text") || Name.starts_with("__text");

    // Collect section data
    SmallString<256> Code;
    raw_svector_ostream VecOS(Code);

    // For virtual sections (BSS), we need to write zeros for their size
    // since PEF doesn't have true BSS - everything goes in the data section
    if (Sec.isVirtualSection()) {
      uint64_t VirtualSize = Asm.getSectionAddressSize(Sec);
      Code.resize(VirtualSize, 0);  // Fill with zeros
    } else {
      Asm.writeSectionData(VecOS, &Sec);
    }

    // Debug: show section size
    uint64_t SectionSize = Asm.getSectionFileSize(Sec);
    llvm::errs() << "DEBUG collectSections: " << Name
                 << " writeSectionData=" << Code.size()
                 << " getSectionFileSize=" << SectionSize
                 << " isCode=" << IsCode
                 << " isVirtual=" << Sec.isVirtualSection() << "\n";

    // Skip sections with no data
    if (Code.size() == 0)
      continue;

    if (IsCode) {
      // Merge into .text section
      if (TextIdx < 0) {
        Sections.emplace_back(".text", &Sec);
        TextIdx = Sections.size() - 1;
        Sections[TextIdx].SectionKind = PEF::kPEFCodeSection;
        Sections[TextIdx].Alignment = Log2(Sec.getAlign());
        Sections[TextIdx].NameOffset = addString(".text");
      }

      // Track offset of this input section in the merged output
      uint64_t CurrentOffset = Sections[TextIdx].Data.size();
      SectionMergeOffset[&Sec] = CurrentOffset;

      // Append data to the text section
      Sections[TextIdx].Data.append(Code.begin(), Code.end());

      // Adjust and add relocations for this sub-section
      for (const auto &Reloc : Relocs) {
        if (Reloc.Section == &Sec ||
            (Reloc.Section && Sec.getName() == Reloc.Section->getName())) {
          Sections[TextIdx].Relocations.emplace_back(CurrentOffset + Reloc.Offset,
                                               Reloc.Symbol, Reloc.Type,
                                               Reloc.Flags, Reloc.Addend);
        }
      }
    } else {
      // Merge into .data section
      if (DataIdx < 0) {
        Sections.emplace_back(".data", &Sec);
        DataIdx = Sections.size() - 1;
        Sections[DataIdx].SectionKind = PEF::kPEFUnpackedDataSection;
        Sections[DataIdx].Alignment = Log2(Sec.getAlign());
        Sections[DataIdx].NameOffset = addString(".data");
      }

      // Track offset of this input section in the merged output
      uint64_t CurrentOffset = Sections[DataIdx].Data.size();
      SectionMergeOffset[&Sec] = CurrentOffset;

      llvm::errs() << "DEBUG: Section " << Name << " merged at offset " << CurrentOffset << "\n";

      // Append data to the data section
      Sections[DataIdx].Data.append(Code.begin(), Code.end());

      // Adjust and add relocations for this sub-section
      for (const auto &Reloc : Relocs) {
        if (Reloc.Section == &Sec ||
            (Reloc.Section && Sec.getName() == Reloc.Section->getName())) {
          Sections[DataIdx].Relocations.emplace_back(CurrentOffset + Reloc.Offset,
                                               Reloc.Symbol, Reloc.Type,
                                               Reloc.Flags, Reloc.Addend);
        }
      }
    }
  }

  // Update lengths for the merged sections
  if (TextIdx >= 0) {
    Sections[TextIdx].UnpackedLength = Sections[TextIdx].Data.size();
    Sections[TextIdx].TotalLength = Sections[TextIdx].UnpackedLength;
    Sections[TextIdx].ContainerLength = Sections[TextIdx].UnpackedLength;
  }
  if (DataIdx >= 0) {
    Sections[DataIdx].UnpackedLength = Sections[DataIdx].Data.size();
    Sections[DataIdx].TotalLength = Sections[DataIdx].UnpackedLength;
    Sections[DataIdx].ContainerLength = Sections[DataIdx].UnpackedLength;
  }
}

void PEFWriter::patchMergeOffsets(MCAssembler &Asm) {
  // After merging sections, we need to patch instruction immediates
  // that reference symbols in merged sections. The assembler wrote
  // the offset within the original section, but we need the offset
  // within the merged section.

  for (auto &Section : Sections) {
    for (const auto &Reloc : Section.Relocations) {
      // Only patch data section relocations (BySectD)
      if (Reloc.Type != PEF::kPEFRelocBySectD)
        continue;

      // Get the target symbol's section
      if (!Reloc.Symbol || Reloc.Symbol->isUndefined() || !Reloc.Symbol->getFragment())
        continue;

      const MCSection *SymSection = Reloc.Symbol->getFragment()->getParent();

      // Find the merge offset for the target section
      auto It = SectionMergeOffset.find(SymSection);
      if (It == SectionMergeOffset.end())
        continue;

      uint64_t MergeOffset = It->second;
      if (MergeOffset == 0)
        continue; // No adjustment needed

      // Get the symbol's offset within its original section
      uint64_t SymbolOffset = Asm.getSymbolOffset(*Reloc.Symbol);

      // The relocation offset points to the 16-bit immediate field of the
      // instruction (offset +2 from instruction start for big-endian PPC).
      // We need to adjust back to read the full 32-bit instruction.
      uint64_t InstrOffset = Reloc.Offset >= 2 ? Reloc.Offset - 2 : Reloc.Offset;
      if (InstrOffset + 4 > Section.Data.size()) {
        llvm::errs() << "ERROR: Relocation offset out of bounds\n";
        continue;
      }

      // Read the current instruction (big-endian)
      uint32_t Instr = (static_cast<uint8_t>(Section.Data[InstrOffset]) << 24) |
                       (static_cast<uint8_t>(Section.Data[InstrOffset + 1]) << 16) |
                       (static_cast<uint8_t>(Section.Data[InstrOffset + 2]) << 8) |
                       static_cast<uint8_t>(Section.Data[InstrOffset + 3]);

      // Extract the 16-bit signed immediate (low 16 bits)
      int16_t OldImm = static_cast<int16_t>(Instr & 0xFFFF);

      // Compute new immediate with merge offset
      int32_t NewImm = OldImm + static_cast<int32_t>(MergeOffset);

      llvm::errs() << "DEBUG patchMergeOffsets: " << Reloc.Symbol->getName()
                   << " at offset " << Reloc.Offset
                   << " OldImm=" << OldImm
                   << " MergeOffset=" << MergeOffset
                   << " NewImm=" << NewImm << "\n";

      // Check for overflow
      if (NewImm > 32767 || NewImm < -32768) {
        llvm::errs() << "ERROR: Immediate overflow after merge offset adjustment\n";
        continue;
      }

      // Update the instruction with new immediate
      Instr = (Instr & 0xFFFF0000) | (static_cast<uint16_t>(NewImm) & 0xFFFF);

      // Write back (big-endian)
      Section.Data[InstrOffset] = (Instr >> 24) & 0xFF;
      Section.Data[InstrOffset + 1] = (Instr >> 16) & 0xFF;
      Section.Data[InstrOffset + 2] = (Instr >> 8) & 0xFF;
      Section.Data[InstrOffset + 3] = Instr & 0xFF;
    }
  }
}

void PEFWriter::collectSymbols(MCAssembler &Asm) {
  uint32_t SymbolIndex = 0;

  for (const MCSymbol &Sym : Asm.symbols()) {
    // Skip temporary symbols
    if (Sym.isTemporary())
      continue;

    // Handle undefined symbols as imports
    if (!Sym.isDefined()) {
      // Create an imported symbol entry
      // For object files, we don't know which library yet - that's determined by the linker
      // Symbol class defaults to TVector for function imports (most common for Mac OS Toolbox)
      PEFSymbolEntry Entry(Sym.getName(), &Sym, 0, -1, false);
      Entry.NameOffset = addString(Entry.Name);
      Entry.SymbolClass = PEF::kPEFTVectorSymbol; // Transition vector for cross-fragment calls
      ImportedSymbols.push_back(Entry);
      SymbolIndexMap[&Sym] = SymbolIndex++;
      continue;
    }

    const auto &Fragment = *Sym.getFragment();
    const auto &Section = *Fragment.getParent();

    // Determine if symbol is in code or data section
    StringRef SectionName = Section.getName();
    bool IsCode = SectionName.starts_with(".text") || SectionName.starts_with("__text");

    // Find the merged section index (0 = .text, 1 = .data typically)
    int16_t SectionIndex = -1;
    for (size_t i = 0; i < Sections.size(); ++i) {
      bool MergedIsCode = (Sections[i].SectionKind == PEF::kPEFCodeSection);
      if (IsCode == MergedIsCode) {
        SectionIndex = i;
        break;
      }
    }

    if (SectionIndex == -1)
      continue;

    // Get symbol's offset within its original section
    uint64_t SymbolOffset = Asm.getSymbolOffset(Sym);

    // Add the section merge offset to get the final offset in the merged section
    auto It = SectionMergeOffset.find(&Section);
    uint64_t MergeOffset = (It != SectionMergeOffset.end()) ? It->second : 0;
    uint64_t Address = SymbolOffset + MergeOffset;

    llvm::errs() << "DEBUG collectSymbols: " << Sym.getName()
                 << " SymbolOffset=" << SymbolOffset
                 << " MergeOffset=" << MergeOffset
                 << " FinalAddress=" << Address << "\n";

    // Export symbols that are not temporary (local labels start with .L)
    bool IsExported = !Sym.isTemporary();

    PEFSymbolEntry Entry(Sym.getName(), &Sym, Address, SectionIndex,
                         IsExported);
    Entry.NameOffset = addString(Entry.Name);
    // Note: SymbolClass is now part of PEFSymbolEntry, not PEFSectionEntry

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
  uint32_t Offset = 40;

  // Section headers: 28 bytes each (including loader section header)
  Offset += (Sections.size() + 1) * 28;

  // Align to 16 bytes for section data
  uint32_t AlignOffset = Offset % 16;
  if (AlignOffset != 0)
    Offset += (16 - AlignOffset);

  // Assign offsets to sections
  for (auto &Section : Sections) {
    // Align each section
    uint32_t SectionAlign = 1u << Section.Alignment;
    uint32_t SectionAlignOffset = Offset % SectionAlign;
    if (SectionAlignOffset != 0)
      Offset += (SectionAlign - SectionAlignOffset);

    // PEF sections use section-relative addressing, not absolute addresses
    // DefaultAddress should always be 0 for each section
    // CFM will relocate based on actual load address at runtime
    Section.DefaultAddress = 0;

    Section.ContainerOffset = Offset;
    Offset += Section.ContainerLength;
  }

  // Note: Don't set FileOffset here - it will be advanced during writing
  // FileOffset remains at 0 for now
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

  // Current version
  write32(0);

  // Number of sections (including loader section) - UInt16!
  write16(Sections.size() + 1); // +1 for loader section

  // Number of instantiated sections (all but loader) - UInt16!
  write16(Sections.size());

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
  // We'll update the sizes and offset after writing the loader section
  write32(addString("loader")); // Name offset
  write32(0);                   // Default address
  write32(0);                   // Total length (will be updated)
  write32(0);                   // Unpacked length (will be updated)
  write32(0);                   // Container length (will be updated)
  write32(0);                   // Container offset (will be updated)
  write8(PEF::kPEFLoaderSection);  // Section kind
  write8(PEF::kPEFGlobalShare);    // Share kind
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

  // Count sections that have relocations
  uint32_t RelocSectionCount = 0;
  for (const auto &Section : Sections) {
    if (!Section.Relocations.empty())
      RelocSectionCount++;
  }

  // Calculate offsets for loader section components
  // Layout: Header (56) | ImportedLibraries | ImportedSymbols | RelocHeaders | RelocInstrs | StringTable | HashTable | KeyTable | ExportTable
  uint32_t ImportedLibrariesOffset = 56; // Right after header
  uint32_t ImportedSymbolsOffset = ImportedLibrariesOffset; // Will be updated if libraries present
  uint32_t RelocHeadersOffset = ImportedSymbolsOffset + (ImportedSymbols.size() * 4); // 4 bytes per imported symbol
  uint32_t RelocInstrOffset = RelocHeadersOffset + (RelocSectionCount * 12); // 12 bytes per reloc header
  uint32_t StringTableOffset = RelocInstrOffset; // Will be updated after writing reloc instructions
  uint32_t HashTableOffset = StringTableOffset + StringTable.size();

  // Align hash table to 4 bytes
  if (HashTableOffset % 4 != 0)
    HashTableOffset += 4 - (HashTableOffset % 4);

  // Loader info header (56 bytes)
  write32(0);  // Main section (-1 if none)
  write32(0);  // Main offset
  write32(-1); // Init section (-1 if none)
  write32(0);  // Init offset
  write32(-1); // Term section (-1 if none)
  write32(0);  // Term offset

  // Number of imported libraries (0 for object files - linker determines this)
  write32(0);

  // Total imported symbol count
  write32(ImportedSymbols.size());

  // Number of relocation sections
  write32(RelocSectionCount);

  // Relocation instructions offset
  write32(RelocInstrOffset);

  // Loader string table offset
  write32(StringTableOffset);

  // Hash slot table offset
  write32(HashTableOffset);

  // Hash slot count (power of 2: 2^0 = 1)
  write32(0);

  // Exported symbol count
  write32(ExportedSymbols.size());

  // Write imported symbols (4 bytes each: class + name offset)
  for (const auto &Sym : ImportedSymbols) {
    uint32_t ClassAndName = PEF::composeImportedSymbol(
        static_cast<uint8_t>(Sym.SymbolClass), Sym.NameOffset);
    write32(ClassAndName);
  }

  // Build all relocation instructions first
  SmallVector<char, 256> RelocInstructions;
  SmallVector<std::tuple<uint16_t, uint32_t, uint32_t>, 8> RelocHeaders; // sectionIndex, relocCount, relocOffset

  for (size_t i = 0; i < Sections.size(); ++i) {
    const auto &Section = Sections[i];
    if (Section.Relocations.empty())
      continue;

    // Generate relocation instructions for this section
    SmallVector<uint16_t, 64> SectionRelocInstrs;

    // Sort relocations by offset
    SmallVector<PEFRelocation, 64> SortedRelocs(Section.Relocations.begin(),
                                                 Section.Relocations.end());
    std::sort(SortedRelocs.begin(), SortedRelocs.end(),
              [](const PEFRelocation &A, const PEFRelocation &B) {
                return A.Offset < B.Offset;
              });

    uint32_t CurrentOffset = 0;
    for (const auto &Reloc : SortedRelocs) {
      // Set position if needed
      if (Reloc.Offset != CurrentOffset) {
        uint32_t NewOffset = Reloc.Offset;
        SectionRelocInstrs.push_back(PEF::composeSetPosition1st(NewOffset));
        SectionRelocInstrs.push_back(PEF::composeSetPosition2nd(NewOffset));
        CurrentOffset = NewOffset;
      }

      // For undefined symbols (imports), emit import relocation
      if (!Reloc.Symbol->isDefined()) {
        // Find import index
        uint32_t ImportIndex = 0;
        for (size_t j = 0; j < ImportedSymbols.size(); ++j) {
          if (ImportedSymbols[j].Symbol == Reloc.Symbol) {
            ImportIndex = j;
            break;
          }
        }

        // Emit large import relocation (22-bit index)
        SectionRelocInstrs.push_back(PEF::composeLgByImport1st(ImportIndex));
        SectionRelocInstrs.push_back(PEF::composeLgByImport2nd(ImportIndex));
        CurrentOffset += 4; // 4-byte pointer
      }
      // For defined symbols, emit section-relative relocation
      else {
        // Determine target section
        const auto &Fragment = *Reloc.Symbol->getFragment();
        const auto &TargetSection = *Fragment.getParent();

        // Find target section index
        int16_t TargetSectionIndex = -1;
        for (size_t j = 0; j < Sections.size(); ++j) {
          // Compare by pointer first, then by name as fallback
          if (Sections[j].Section == &TargetSection ||
              TargetSection.getName() == Sections[j].Section->getName()) {
            TargetSectionIndex = j;
            break;
          }
        }
        // If section not found, check if it's a BSS/common symbol (section named after symbol)
        // and map it to .data section
        if (TargetSectionIndex < 0 && !TargetSection.isText()) {
          for (size_t j = 0; j < Sections.size(); ++j) {
            if (Sections[j].Section->getName().starts_with(".data") ||
                Sections[j].Section->getName().starts_with("__data")) {
              TargetSectionIndex = j;
              break;
            }
          }
        }

        // Emit section relocation (run of 1)
        if (TargetSectionIndex >= 0) {
          uint8_t TargetSectionKind = Sections[TargetSectionIndex].SectionKind;
          llvm::errs() << "DEBUG EMIT: Symbol " << Reloc.Symbol->getName()
                       << " TargetSectionIndex=" << TargetSectionIndex
                       << " TargetSectionKind=" << (unsigned)TargetSectionKind
                       << " kPEFCodeSection=" << (unsigned)PEF::kPEFCodeSection << "\n";
          if (TargetSectionKind == PEF::kPEFCodeSection) {
            llvm::errs() << "  → Emitting BySectC(0) for run length 1\n";
            SectionRelocInstrs.push_back(PEF::composeBySectC(0)); // operand=0 means runLength=1
          } else {
            llvm::errs() << "  → Emitting BySectD(0) for run length 1\n";
            SectionRelocInstrs.push_back(PEF::composeBySectD(0)); // operand=0 means runLength=1
          }
          CurrentOffset += 4;
        }
      }
    }

    // Save header info for later
    uint32_t RelocInstrCount = SectionRelocInstrs.size();
    uint32_t FirstRelocOffset = RelocInstructions.size();
    RelocHeaders.emplace_back(i, RelocInstrCount, FirstRelocOffset); // Store instruction count, not byte count!

    // Append instructions to buffer
    for (uint16_t Instr : SectionRelocInstrs) {
      RelocInstructions.push_back((Instr >> 8) & 0xFF);
      RelocInstructions.push_back(Instr & 0xFF);
    }
  }

  // Now write the LoaderRelocationHeaders
  // The RelocInstrOffset already points to where the first instruction will be written
  // RelocOffset is the byte offset within the RelocInstructions buffer
  for (const auto &[SectionIndex, RelocCount, RelocOffset] : RelocHeaders) {
    errs() << "DEBUG: Writing reloc header: SectionIndex=" << SectionIndex
           << ", RelocCount=" << RelocCount << ", RelocOffset=" << RelocOffset
           << ", RelocInstrOffset=" << RelocInstrOffset << "\n";
    write16(SectionIndex);  // Section index
    write16(0);  // Reserved
    write32(RelocCount); // Number of relocation instructions
    write32(RelocInstrOffset + RelocOffset); // Offset from start of loader section
  }

  // Write relocation instructions
  writeBytes(ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(RelocInstructions.data()),
      RelocInstructions.size()));

  // Update actual string table offset
  uint64_t ActualStringTableOffset = FileOffset - LoaderSectionStart;
  char StringTableOffsetBE[4];
  support::endian::write<uint32_t>(StringTableOffsetBE, ActualStringTableOffset,
                                    llvm::endianness::big);
  OS.pwrite(StringTableOffsetBE, 4, LoaderSectionStart + 40); // LoaderStringsOffset is at offset 40

  // Write string table
  writeBytes(ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(StringTable.data()),
      StringTable.size()));

  // Align to 4 bytes
  alignTo(4);

  // Record actual hash table offset after alignment
  uint32_t ActualHashTableOffset = FileOffset - LoaderSectionStart;

  // Update ExportHashOffset in loader info header with actual value
  // Convert to big-endian and write
  char HashOffsetBE[4];
  support::endian::write<uint32_t>(HashOffsetBE, ActualHashTableOffset,
                                    llvm::endianness::big);
  OS.pwrite(HashOffsetBE, 4, LoaderSectionStart + 44); // ExportHashOffset is at offset 44

  // Write hash table (1 slot: chain count + first index)
  // Simple hash: all symbols in one chain starting at index 0
  uint32_t HashSlot = PEF::composeHashSlot(ExportedSymbols.size(), 0);
  write32(HashSlot);

  // Write key table (one 4-byte hash value per exported symbol)
  for (size_t i = 0; i < ExportedSymbols.size(); ++i) {
    // Simple hash: just use symbol index
    write32(i);
  }

  // Write exported symbols
  for (const auto &Sym : ExportedSymbols) {
    // Compose ClassAndName field: class (8 bits high) + name offset (24 bits low)
    uint32_t ClassAndName = PEF::composeExportedSymbol(
        static_cast<uint8_t>(Sym.SymbolClass), Sym.NameOffset);
    write32(ClassAndName);
    write32(Sym.Value);
    write16(Sym.SectionIndex);
  }

  // Align to 4 bytes
  alignTo(4);

  uint64_t LoaderSectionEnd = FileOffset;
  uint32_t LoaderSize = LoaderSectionEnd - LoaderSectionStart;
  uint32_t LoaderOffset = LoaderSectionStart;

  // Update loader section header (sizes and offset) in big-endian format
  uint64_t LoaderHeaderOffset = 40 + (Sections.size() * 28);

  char LoaderSizeBE[4], LoaderOffsetBE[4];
  support::endian::write<uint32_t>(LoaderSizeBE, LoaderSize, llvm::endianness::big);
  support::endian::write<uint32_t>(LoaderOffsetBE, LoaderOffset, llvm::endianness::big);

  OS.pwrite(LoaderSizeBE, 4, LoaderHeaderOffset + 8);   // Total length
  OS.pwrite(LoaderSizeBE, 4, LoaderHeaderOffset + 12);  // Unpacked length
  OS.pwrite(LoaderSizeBE, 4, LoaderHeaderOffset + 16);  // Container length
  OS.pwrite(LoaderOffsetBE, 4, LoaderHeaderOffset + 20); // Container offset
}

void PEFWriter::writeObject(MCAssembler &Asm,
                            const std::vector<PEFObjectWriter::StoredRelocation> &Relocs) {
  // Collect all sections and symbols
  collectSections(Asm, Relocs);

  // Patch instruction immediates to account for merged section offsets.
  // When multiple sections (e.g., BSS sections) are merged into .data,
  // each symbol's offset within the merged section must be encoded in
  // the instruction. The linker will then add its adjustment (import table,
  // padding, TVector) on top of this base offset.
  patchMergeOffsets(Asm);

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

void PEFObjectWriter::reset() {
  // Clear relocations when resetting
  Relocations.clear();
}

void PEFObjectWriter::executePostLayoutBinding(MCAssembler &Asm) {}

void PEFObjectWriter::recordRelocation(MCAssembler &Asm,
                                       const MCFragment *Fragment,
                                       const MCFixup &Fixup, MCValue Target,
                                       uint64_t &FixedValue) {
  // Get the symbol being referenced
  const MCSymbolRefExpr *RefA = Target.getSymA();
  if (!RefA)
    return; // No symbol reference, nothing to relocate

  const MCSymbol *Symbol = &RefA->getSymbol();
  const MCSection *Section = Fragment->getParent();

  // Debug: show what we're processing
  llvm::errs() << "DEBUG recordRelocation: Symbol=" << Symbol->getName()
               << " HasRefB=" << (Target.getSymB() != nullptr) << "\n";

  // TEMPORARILY DISABLED: Check if this is a TOC-relative reference (SymA - .LTOC)
  // For r2-relative loads/stores, we need to emit a BySectD relocation
  // so the linker can adjust the offset when it prepends import tables
  const MCSymbolRefExpr *RefB = Target.getSymB();
  if (RefB) {
    const MCSymbol &SymB = RefB->getSymbol();
    llvm::errs() << "DEBUG: RefB symbol=" << SymB.getName() << "\n";
    if (SymB.getName() == ".LTOC") {
      // TOC-relative reference: SymA - .LTOC
      // This is used for r2-relative loads/stores of data section symbols
      // We need to emit a BySectD relocation so the linker can adjust
      // the offset when it prepends import tables to the data section

      // Compute the actual TOC-relative offset: SymA - .LTOC + constant
      if (!Symbol->isUndefined() && Symbol->getFragment()) {
        uint64_t SymbolOffset = Asm.getSymbolOffset(*Symbol);
        uint64_t LTOCOffset = Asm.getSymbolOffset(SymB);
        FixedValue = SymbolOffset - LTOCOffset + Target.getConstant();

        llvm::errs() << "DEBUG: TOC-relative reference to " << Symbol->getName()
                     << " SymOffset=" << SymbolOffset
                     << " LTOCOffset=" << LTOCOffset
                     << " FixedValue=" << FixedValue << "\n";

        // DISABLED: Don't emit the relocation to test if this is causing crashes
        /*
        // Calculate fixup offset within section
        uint64_t FragmentOffset = Asm.getFragmentOffset(*Fragment);
        uint64_t FixupOffset = FragmentOffset + Fixup.getOffset();
        int64_t Addend = Target.getConstant();

        llvm::errs() << "DEBUG: TOC-relative reference to " << Symbol->getName()
                     << " offset=" << SymbolOffset << " - emitting BySectD\n";

        // Store a BySectD relocation for the linker to adjust
        StoredRelocation Reloc;
        Reloc.Section = Section;
        Reloc.Offset = FixupOffset;
        Reloc.Symbol = Symbol;
        Reloc.Type = PEF::kPEFRelocBySectD;
        Reloc.Flags = 0;
        Reloc.Addend = Addend;
        Relocations.push_back(Reloc);
        */
      }
      return;
    }
  }

  // Compute the offset of this fixup within its section
  uint64_t FragmentOffset = Asm.getFragmentOffset(*Fragment);
  uint64_t FixupOffset = FragmentOffset + Fixup.getOffset();

  // Determine relocation type based on fixup kind
  uint16_t RelocType = PEF::kPEFRelocBySectC; // Default to code section relocation
  uint16_t Flags = 0;
  int64_t Addend = Target.getConstant();

  unsigned Kind = Fixup.getKind();

  if (Kind == FK_Data_4) {
    // 32-bit absolute data reference
    // Type will be determined by linker based on target section
    RelocType = PEF::kPEFRelocBySectC;
  } else if (Kind >= FirstTargetFixupKind) {
    // PowerPC-specific fixups
    unsigned PPCKind = Kind - FirstTargetFixupKind;
    switch (PPCKind) {
      case 0: // fixup_ppc_br24 - 24-bit PC-relative branch
        RelocType = PEF::kPEFRelocBySectC;
        Flags = 1; // Mark as PC-relative
        break;
      case 6: // fixup_ppc_half16 - 16-bit immediate
        RelocType = PEF::kPEFRelocBySectC;
        break;
      default:
        // Other fixup types - use default
        break;
    }
  }

  // Check if this is an internal reference (symbol defined in same section)
  // For internal PC-relative branches, we can calculate the offset directly
  // instead of emitting a relocation for the linker to resolve.
  if (!Symbol->isUndefined() && Symbol->getFragment()) {
    const MCSection *SymSection = Symbol->getFragment()->getParent();
    if (SymSection == Section && (Flags & 1)) {  // Flags & 1 means PC-relative
      // Internal PC-relative branch - calculate actual offset
      uint64_t SymbolOffset = Asm.getSymbolOffset(*Symbol);
      int64_t Offset = static_cast<int64_t>(SymbolOffset) -
                       static_cast<int64_t>(FixupOffset) + Addend;
      FixedValue = static_cast<uint64_t>(Offset);

      // Don't store this relocation - it's already resolved
      return;
    }

    // For code section fixups referencing data section symbols,
    // we need to emit BySectD relocations so the linker can adjust
    // the immediate field to account for import table, padding, and TVector
    // that get prepended to the data section.
    //
    // For code section fixups referencing code section symbols (internal calls),
    // we don't need relocations since PC-relative offsets are correct.
    if (Section->isText() && SymSection->isText()) {
      // Code-to-code reference - compute offset but don't emit relocation
      uint64_t SymbolOffset = Asm.getSymbolOffset(*Symbol);
      FixedValue = SymbolOffset;
      llvm::errs() << "DEBUG: Code-to-code fixup - setting FixedValue=" << SymbolOffset
                   << " for symbol " << Symbol->getName() << " (no relocation)\n";
      return;
    }

    // Code-to-data references need BySectD relocations
    // The linker will patch the immediate field of addi/lwz/stw instructions

    // Determine relocation type based on target section
    // BySectD = target is in data section (add data section base at runtime)
    // BySectC = target is in code section (add code section base at runtime)
    // This applies regardless of which section the reference is FROM
    if (!SymSection->isText()) {
      // Target is in data section - need BySectD
      RelocType = PEF::kPEFRelocBySectD;
      llvm::errs() << "DEBUG: Setting BySectD for symbol " << Symbol->getName()
                   << " (target is in data section)\n";
    } else {
      // Target is in code section - need BySectC
      RelocType = PEF::kPEFRelocBySectC;
      llvm::errs() << "DEBUG: Setting BySectC for symbol " << Symbol->getName()
                   << " (target is in code section)\n";
    }
  }

  // Compute the value to write to the object file
  // PEF relocations work differently from ELF/Mach-O:
  // - The data section contains section-relative offsets
  // - CFM adds the section base at runtime: absolute_addr = base + offset
  // For defined symbols, use the symbol's offset within its section
  // For undefined symbols (imports), use 0 as placeholder
  if (!Symbol->isUndefined() && Symbol->getFragment()) {
    // Get the symbol's offset within its containing section
    uint64_t SymbolOffset = Asm.getSymbolOffset(*Symbol);
    FixedValue = SymbolOffset;
    llvm::errs() << "DEBUG: Setting FixedValue=" << SymbolOffset
                 << " for symbol " << Symbol->getName() << "\n";
  } else {
    // External reference - will be resolved by linker
    FixedValue = 0;
  }

  // Store the relocation for later processing
  StoredRelocation Reloc;
  Reloc.Section = Section;
  Reloc.Offset = FixupOffset;
  Reloc.Symbol = Symbol;
  Reloc.Type = RelocType;
  Reloc.Flags = Flags;
  Reloc.Addend = Addend;

  Relocations.push_back(Reloc);
}

bool PEFObjectWriter::isSymbolRefDifferenceFullyResolvedImpl(
    const MCAssembler &Asm, const MCSymbol &SymA, const MCFragment &FB,
    bool InSet, bool IsPCRel) const {
  // Conservative: assume symbols in different sections need relocations
  if (SymA.isUndefined())
    return false;

  if (!SymA.getFragment())
    return false;

  const MCSection *SecA = SymA.getFragment()->getParent();
  const MCSection *SecB = FB.getParent();

  // For PEF, only fold if both are in the same section
  // AND both are in the code section (for internal branches)
  // TOC-relative references (data section) should NOT be folded
  // because the linker may adjust the data section layout
  if (SecA != SecB)
    return false;

  // Only fold code section references (internal branches)
  // Data section differences need to go through recordRelocation
  // so we can compute the correct TOC-relative offset
  if (!SecA->isText())
    return false;

  return true;
}

uint64_t PEFObjectWriter::writeObject(MCAssembler &Asm) {
  auto &Writer =
      static_cast<MCPEFObjectTargetWriter &>(*this->TargetObjectWriter);
  // PEF is always big-endian (PowerPC)
  PEFWriter W(OS, Writer);
  W.writeObject(Asm, Relocations);
  return 0;
}

std::unique_ptr<MCObjectWriter>
llvm::createPEFObjectWriter(std::unique_ptr<MCPEFObjectTargetWriter> MOTW,
                            raw_pwrite_stream &OS, bool IsLittleEndian) {
  return std::make_unique<PEFObjectWriter>(std::move(MOTW), OS,
                                           IsLittleEndian);
}
