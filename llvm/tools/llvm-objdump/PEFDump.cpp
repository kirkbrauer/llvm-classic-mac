//===-- PEFDump.cpp - PEF-specific dumper for llvm-objdump -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the PEF-specific dumper for llvm-objdump.
//
//===----------------------------------------------------------------------===//

#include "PEFDump.h"
#include "llvm-objdump.h"
#include "llvm/BinaryFormat/PEF.h"
#include "llvm/Object/PEFObjectFile.h"
#include "llvm/Support/Format.h"

using namespace llvm;
using namespace llvm::object;
using namespace llvm::PEF;

namespace {

class PEFDumper : public objdump::Dumper {
  const PEFObjectFile &Obj;

public:
  PEFDumper(const PEFObjectFile &O) : Dumper(O), Obj(O) {}
  void printPrivateHeaders() override;
};

} // namespace

std::unique_ptr<objdump::Dumper>
objdump::createPEFDumper(const object::PEFObjectFile &Obj) {
  return std::make_unique<PEFDumper>(Obj);
}

void PEFDumper::printPrivateHeaders() {
  const ContainerHeader &Hdr = Obj.getHeader();

  outs() << "PEF Container Header:\n";
  outs() << format("  Magic:        'Joy!' 'peff' (0x%08X 0x%08X)\n",
                   Hdr.Tag1, Hdr.Tag2);

  // Print architecture
  std::string ArchName;
  if (Hdr.Architecture == kPEFPowerPCArch)
    ArchName = "PowerPC ('pwpc')";
  else if (Hdr.Architecture == kPEFM68KArch)
    ArchName = "68K ('m68k')";
  else
    ArchName = "Unknown";

  outs() << format("  Architecture: 0x%08X (%s)\n",
                   Hdr.Architecture, ArchName.c_str());
  outs() << format("  Format Version: %u\n", Hdr.FormatVersion);
  outs() << format("  Date/Time Stamp: 0x%08X\n", Hdr.DateTimeStamp);
  outs() << format("  Old Def Version: 0x%08X\n", Hdr.OldDefVersion);
  outs() << format("  Old Imp Version: 0x%08X\n", Hdr.OldImpVersion);
  outs() << format("  Current Version: 0x%08X\n", Hdr.CurrentVersion);
  outs() << format("  Section Count: %u\n", Hdr.SectionCount);
  outs() << format("  Inst Section Count: %u\n", Hdr.InstSectionCount);

  // Print section headers
  outs() << "\nPEF Section Headers:\n";
  for (unsigned I = 0; I < Obj.getSectionCount(); ++I) {
    Expected<SectionHeader> HdrOrErr = Obj.getSectionHeader(I);
    if (!HdrOrErr) {
      outs() << format("  Section %u: Error reading section header\n", I);
      consumeError(HdrOrErr.takeError());
      continue;
    }

    const SectionHeader &SecHdr = *HdrOrErr;

    // Get section name
    section_iterator SecIter = Obj.section_begin();
    std::advance(SecIter, I);
    Expected<StringRef> NameOrErr =
        Obj.getSectionName(SecIter->getRawDataRefImpl());
    StringRef SecName = NameOrErr ? *NameOrErr : "<unknown>";

    outs() << format("  Section %u: %s\n", I, SecName.str().c_str());

    // Print section kind
    std::string KindName;
    switch (SecHdr.SectionKind) {
    case kPEFCodeSection:
      KindName = "Code";
      break;
    case kPEFUnpackedDataSection:
      KindName = "Unpacked Data";
      break;
    case kPEFPatternDataSection:
      KindName = "Pattern Data";
      break;
    case kPEFConstantSection:
      KindName = "Constant";
      break;
    case kPEFLoaderSection:
      KindName = "Loader";
      break;
    case kPEFDebugSection:
      KindName = "Debug";
      break;
    case kPEFExecutableDataSection:
      KindName = "Executable Data";
      break;
    case kPEFExceptionSection:
      KindName = "Exception";
      break;
    case kPEFTracebackSection:
      KindName = "Traceback";
      break;
    default:
      KindName = "Unknown";
      break;
    }

    outs() << format("    Kind: %s (%u)\n", KindName.c_str(),
                     SecHdr.SectionKind);

    // Print share kind
    std::string ShareName;
    switch (SecHdr.ShareKind) {
    case kPEFProcessShare:
      ShareName = "Process";
      break;
    case kPEFGlobalShare:
      ShareName = "Global";
      break;
    case kPEFProtectedShare:
      ShareName = "Protected";
      break;
    default:
      ShareName = "Unknown";
      break;
    }

    outs() << format("    Share: %s (%u)\n", ShareName.c_str(),
                     SecHdr.ShareKind);
    outs() << format("    Default Address: 0x%08X\n", SecHdr.DefaultAddress);
    outs() << format("    Total Length: %u bytes\n", SecHdr.TotalLength);
    outs() << format("    Unpacked Length: %u bytes\n", SecHdr.UnpackedLength);
    outs() << format("    Container Length: %u bytes\n",
                     SecHdr.ContainerLength);
    outs() << format("    Container Offset: 0x%08X\n",
                     SecHdr.ContainerOffset);
    outs() << format("    Alignment: %u bytes\n", 1U << SecHdr.Alignment);

    // Print loader section details if this is a loader section
    if (SecHdr.SectionKind == kPEFLoaderSection) {
      Expected<LoaderInfoHeader> LoaderInfoOrErr = Obj.getLoaderInfoHeader();
      if (LoaderInfoOrErr) {
        const LoaderInfoHeader &LoaderInfo = *LoaderInfoOrErr;

        outs() << "    Loader Info:\n";
        outs() << format("      Main Section: %d\n", LoaderInfo.MainSection);
        outs() << format("      Main Offset: 0x%08X\n", LoaderInfo.MainOffset);
        outs() << format("      Init Section: %d\n", LoaderInfo.InitSection);
        outs() << format("      Init Offset: 0x%08X\n", LoaderInfo.InitOffset);
        outs() << format("      Term Section: %d\n", LoaderInfo.TermSection);
        outs() << format("      Term Offset: 0x%08X\n", LoaderInfo.TermOffset);
        outs() << format("      Imported Library Count: %u\n",
                         LoaderInfo.ImportedLibraryCount);
        outs() << format("      Total Imported Symbol Count: %u\n",
                         LoaderInfo.TotalImportedSymbolCount);
        outs() << format("      Reloc Section Count: %u\n",
                         LoaderInfo.RelocSectionCount);
        outs() << format("      Exported Symbol Count: %u\n",
                         LoaderInfo.ExportedSymbolCount);
      } else {
        outs() << "    Loader Info: Error reading loader header\n";
        consumeError(LoaderInfoOrErr.takeError());
      }
    }
  }
}

void objdump::printPEFFileHeader(const PEFObjectFile *Obj) {
  const ContainerHeader &Hdr = Obj->getHeader();

  outs() << "architecture: ";
  if (Hdr.Architecture == kPEFPowerPCArch)
    outs() << "ppc\n";
  else if (Hdr.Architecture == kPEFM68KArch)
    outs() << "m68k\n";
  else
    outs() << "unknown\n";

  // Print start address from loader info if available
  Expected<LoaderInfoHeader> LoaderInfoOrErr = Obj->getLoaderInfoHeader();
  if (LoaderInfoOrErr) {
    const LoaderInfoHeader &LoaderInfo = *LoaderInfoOrErr;
    if (LoaderInfo.MainSection >= 0) {
      Expected<SectionHeader> SecHdrOrErr =
          Obj->getSectionHeader(LoaderInfo.MainSection);
      if (SecHdrOrErr) {
        uint64_t StartAddr =
            SecHdrOrErr->DefaultAddress + LoaderInfo.MainOffset;
        outs() << format("start address: 0x%08llx\n",
                         (unsigned long long)StartAddr);
      }
    }
  }
}

Error objdump::getPEFRelocationValueString(const PEFObjectFile *Obj,
                                           const RelocationRef &RelRef,
                                           SmallVectorImpl<char> &Result) {
  // PEF relocations are bytecode-based and complex
  // For now, provide basic information
  symbol_iterator SI = RelRef.getSymbol();
  if (SI != Obj->symbol_end()) {
    Expected<StringRef> SymNameOrErr = SI->getName();
    if (!SymNameOrErr)
      return SymNameOrErr.takeError();
    StringRef SymName = *SymNameOrErr;
    Result.append(SymName.begin(), SymName.end());
  } else {
    // No symbol, show relocation type
    SmallVector<char, 32> TypeName;
    RelRef.getTypeName(TypeName);
    Result.append(TypeName.begin(), TypeName.end());
  }

  return Error::success();
}
