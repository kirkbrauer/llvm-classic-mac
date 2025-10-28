//===-- PEFDumper.cpp - PEF dumping utility --------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements a PEF (Preferred Executable Format) specific dumper
// for llvm-readobj.
//
//===----------------------------------------------------------------------===//

#include "ObjDumper.h"
#include "llvm-readobj.h"
#include "llvm/BinaryFormat/PEF.h"
#include "llvm/Object/PEFObjectFile.h"
#include "llvm/Support/ScopedPrinter.h"

using namespace llvm;
using namespace object;
using namespace PEF;

namespace {

class PEFDumper : public ObjDumper {
public:
  PEFDumper(const PEFObjectFile &Obj, ScopedPrinter &Writer)
      : ObjDumper(Writer, Obj.getFileName()), Obj(Obj) {}

  void printFileHeaders() override;
  void printSectionHeaders() override;
  void printRelocations() override;
  void printSymbols(bool ExtraSymInfo) override;
  void printDynamicSymbols() override;
  void printUnwindInfo() override;
  void printStackMap() const override;
  void printNeededLibraries() override;

private:
  void printContainerHeader();
  void printSectionHeader(const SectionHeader &Hdr, unsigned Index);
  void printLoaderSection();

  const PEFObjectFile &Obj;
};

} // end anonymous namespace

void PEFDumper::printFileHeaders() {
  printContainerHeader();
}

void PEFDumper::printContainerHeader() {
  DictScope DS(W, "PEFContainerHeader");

  const ContainerHeader &Hdr = Obj.getHeader();

  W.printHex("Tag1", Hdr.Tag1);
  W.printHex("Tag2", Hdr.Tag2);

  // Print architecture name
  std::string ArchName;
  if (Hdr.Architecture == kPEFPowerPCArch)
    ArchName = "PowerPC ('pwpc')";
  else if (Hdr.Architecture == kPEFM68KArch)
    ArchName = "68K ('m68k')";
  else
    ArchName = "Unknown";

  W.printHex("Architecture", Hdr.Architecture);
  W.printString("ArchitectureName", ArchName);
  W.printNumber("FormatVersion", Hdr.FormatVersion);
  W.printHex("DateTimeStamp", Hdr.DateTimeStamp);
  W.printHex("OldDefVersion", Hdr.OldDefVersion);
  W.printHex("OldImpVersion", Hdr.OldImpVersion);
  W.printHex("CurrentVersion", Hdr.CurrentVersion);
  W.printNumber("SectionCount", Hdr.SectionCount);
  W.printNumber("InstSectionCount", Hdr.InstSectionCount);
  W.printHex("ReservedA", Hdr.ReservedA);
}

void PEFDumper::printSectionHeaders() {
  ListScope LS(W, "Sections");

  for (unsigned I = 0; I < Obj.getSectionCount(); ++I) {
    Expected<SectionHeader> HdrOrErr = Obj.getSectionHeader(I);
    if (!HdrOrErr) {
      reportError(HdrOrErr.takeError(), Obj.getFileName());
      continue;
    }

    printSectionHeader(*HdrOrErr, I);
  }
}

void PEFDumper::printSectionHeader(const SectionHeader &Hdr, unsigned Index) {
  DictScope DS(W, "Section");

  W.printNumber("Index", Index);

  // Get section name
  section_iterator SecIter = Obj.section_begin();
  std::advance(SecIter, Index);
  Expected<StringRef> NameOrErr = Obj.getSectionName(SecIter->getRawDataRefImpl());
  if (NameOrErr)
    W.printString("Name", *NameOrErr);
  else
    reportError(NameOrErr.takeError(), Obj.getFileName());

  // Print section kind
  std::string KindName;
  switch (Hdr.SectionKind) {
  case kPEFCodeSection: KindName = "Code"; break;
  case kPEFUnpackedDataSection: KindName = "Unpacked Data"; break;
  case kPEFPatternDataSection: KindName = "Pattern Data"; break;
  case kPEFConstantSection: KindName = "Constant"; break;
  case kPEFLoaderSection: KindName = "Loader"; break;
  case kPEFDebugSection: KindName = "Debug"; break;
  case kPEFExecutableDataSection: KindName = "Executable Data"; break;
  case kPEFExceptionSection: KindName = "Exception"; break;
  case kPEFTracebackSection: KindName = "Traceback"; break;
  default: KindName = "Unknown"; break;
  }

  W.printNumber("SectionKind", Hdr.SectionKind);
  W.printString("SectionKindName", KindName);

  // Print share kind
  std::string ShareName;
  switch (Hdr.ShareKind) {
  case kPEFProcessShare: ShareName = "Process"; break;
  case kPEFGlobalShare: ShareName = "Global"; break;
  case kPEFProtectedShare: ShareName = "Protected"; break;
  default: ShareName = "Unknown"; break;
  }

  W.printNumber("ShareKind", Hdr.ShareKind);
  W.printString("ShareKindName", ShareName);

  W.printHex("DefaultAddress", Hdr.DefaultAddress);
  W.printNumber("TotalLength", Hdr.TotalLength);
  W.printNumber("UnpackedLength", Hdr.UnpackedLength);
  W.printNumber("ContainerLength", Hdr.ContainerLength);
  W.printHex("ContainerOffset", Hdr.ContainerOffset);
  W.printNumber("Alignment", 1ULL << Hdr.Alignment);

  // Print loader section details if this is a loader section
  if (Hdr.SectionKind == kPEFLoaderSection) {
    printLoaderSection();
  }
}

void PEFDumper::printLoaderSection() {
  Expected<LoaderInfoHeader> LoaderInfoOrErr = Obj.getLoaderInfoHeader();
  if (!LoaderInfoOrErr) {
    reportError(LoaderInfoOrErr.takeError(), Obj.getFileName());
    return;
  }

  LoaderInfoHeader LoaderInfo = *LoaderInfoOrErr;

  DictScope LDS(W, "LoaderInfo");
  W.printNumber("MainSection", LoaderInfo.MainSection);
  W.printHex("MainOffset", LoaderInfo.MainOffset);
  W.printNumber("InitSection", LoaderInfo.InitSection);
  W.printHex("InitOffset", LoaderInfo.InitOffset);
  W.printNumber("TermSection", LoaderInfo.TermSection);
  W.printHex("TermOffset", LoaderInfo.TermOffset);
  W.printNumber("ImportedLibraryCount", LoaderInfo.ImportedLibraryCount);
  W.printNumber("TotalImportedSymbolCount", LoaderInfo.TotalImportedSymbolCount);
  W.printNumber("RelocSectionCount", LoaderInfo.RelocSectionCount);
  W.printHex("RelocInstrOffset", LoaderInfo.RelocInstrOffset);
  W.printHex("LoaderStringsOffset", LoaderInfo.LoaderStringsOffset);
  W.printHex("ExportHashOffset", LoaderInfo.ExportHashOffset);
  W.printNumber("ExportHashTablePower", LoaderInfo.ExportHashTablePower);
  W.printNumber("ExportedSymbolCount", LoaderInfo.ExportedSymbolCount);
}

void PEFDumper::printRelocations() {
  // TODO: Implement when relocation support is added to PEFObjectFile
  W.printString("Relocations", "Not yet implemented");
}

void PEFDumper::printSymbols(bool ExtraSymInfo) {
  ListScope LS(W, "Symbols");

  for (const SymbolRef &Sym : Obj.symbols()) {
    DictScope SS(W, "Symbol");

    Expected<StringRef> NameOrErr = Sym.getName();
    if (NameOrErr)
      W.printString("Name", *NameOrErr);
    else
      reportError(NameOrErr.takeError(), Obj.getFileName());

    Expected<uint64_t> AddressOrErr = Sym.getAddress();
    if (AddressOrErr)
      W.printHex("Value", *AddressOrErr);
    else
      reportError(AddressOrErr.takeError(), Obj.getFileName());

    Expected<SymbolRef::Type> TypeOrErr = Sym.getType();
    if (TypeOrErr) {
      std::string TypeName;
      switch (*TypeOrErr) {
      case SymbolRef::ST_Function: TypeName = "Function"; break;
      case SymbolRef::ST_Data: TypeName = "Data"; break;
      case SymbolRef::ST_Unknown: TypeName = "Unknown"; break;
      default: TypeName = "Other"; break;
      }
      W.printString("Type", TypeName);
    } else {
      reportError(TypeOrErr.takeError(), Obj.getFileName());
    }

    Expected<section_iterator> SectionOrErr = Sym.getSection();
    if (SectionOrErr) {
      section_iterator SecIter = *SectionOrErr;
      if (SecIter != Obj.section_end()) {
        Expected<StringRef> SecNameOrErr = SecIter->getName();
        if (SecNameOrErr)
          W.printString("Section", *SecNameOrErr);
        else
          reportError(SecNameOrErr.takeError(), Obj.getFileName());
      }
    } else {
      reportError(SectionOrErr.takeError(), Obj.getFileName());
    }
  }
}

void PEFDumper::printDynamicSymbols() {
  // PEF doesn't have a separate dynamic symbol table
  // All exported symbols are already shown in printSymbols
}

void PEFDumper::printUnwindInfo() {
  // PEF doesn't use standard unwind info
}

void PEFDumper::printStackMap() const {
  // Not applicable to PEF
}

void PEFDumper::printNeededLibraries() {
  // TODO: Implement by reading imported libraries from loader section
  W.printString("NeededLibraries", "Not yet implemented");
}

namespace llvm {

std::unique_ptr<ObjDumper> createPEFDumper(const PEFObjectFile &Obj,
                                           ScopedPrinter &Writer) {
  return std::make_unique<PEFDumper>(Obj, Writer);
}

} // end namespace llvm
