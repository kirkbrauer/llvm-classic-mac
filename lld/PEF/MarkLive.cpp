//===- MarkLive.cpp -------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements --gc-sections, which removes unused sections from the
// output. Unused sections are those not reachable from GC roots (entry point,
// exported symbols). The implementation uses a mark-sweep algorithm:
//
// 1. Mark all sections as dead (default state)
// 2. Starting from GC roots, traverse relocations to mark reachable sections
// 3. Sections not marked live are filtered out before layout
//
//===----------------------------------------------------------------------===//

#include "MarkLive.h"
#include "Config.h"
#include "InputSection.h"
#include "OutputSection.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "lld/Common/ErrorHandler.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace lld;
using namespace lld::pef;

namespace {

class MarkLiveImpl {
public:
  MarkLiveImpl(std::vector<OutputSection *> &outputSections, SymbolTable *symtab)
      : outputSections(outputSections), symtab(symtab) {}

  void run();

private:
  void enqueue(InputSection *sec);
  void markSymbol(Symbol *sym);
  void mark();
  InputSection *getSectionForSymbol(Defined *sym);

  std::vector<OutputSection *> &outputSections;
  SymbolTable *symtab;
  SmallVector<InputSection *, 256> queue;
};

} // namespace

// Find the InputSection containing a defined symbol
InputSection *MarkLiveImpl::getSectionForSymbol(Defined *sym) {
  int16_t secIdx = sym->getSectionIndex();
  if (secIdx < 0)
    return nullptr; // Absolute symbol

  InputFile *symFile = sym->getFile();

  // Search all output sections for the matching input section
  for (OutputSection *osec : outputSections) {
    for (InputSection *isec : osec->getInputSections()) {
      if (isec->getFile() == symFile &&
          isec->getIndex() == static_cast<unsigned>(secIdx))
        return isec;
    }
  }
  return nullptr;
}

void MarkLiveImpl::enqueue(InputSection *sec) {
  if (!sec || sec->isLive())
    return;
  sec->markLive();
  queue.push_back(sec);
}

void MarkLiveImpl::markSymbol(Symbol *sym) {
  if (auto *d = dyn_cast_or_null<Defined>(sym))
    enqueue(getSectionForSymbol(d));
}

void MarkLiveImpl::run() {
  // Mark all sections as dead initially
  for (OutputSection *osec : outputSections)
    for (InputSection *isec : osec->getInputSections())
      isec->markDead();

  // GC Roots:

  // 1. Entry point symbol - this is the primary root
  Symbol *entrySym = symtab->find(config->entry);
  if (entrySym) {
    markSymbol(entrySym);
    if (config->verbose && entrySym->isDefined()) {
      errorHandler().outs() << "GC root: entry point '" << config->entry << "'\n";
    }
  }

  // 2. Exported symbols (if --export-dynamic)
  if (config->exportDynamic) {
    for (Defined *sym : symtab->getDefinedSymbols()) {
      markSymbol(sym);
    }
  }

  // Traverse the reference graph
  mark();

  // Report statistics
  if (config->verbose) {
    unsigned liveCount = 0, deadCount = 0;
    for (OutputSection *osec : outputSections) {
      for (InputSection *isec : osec->getInputSections()) {
        if (isec->isLive())
          liveCount++;
        else
          deadCount++;
      }
    }
    errorHandler().outs() << "GC: " << liveCount << " live sections, "
                          << deadCount << " dead sections\n";
  }
}

void MarkLiveImpl::mark() {
  while (!queue.empty()) {
    InputSection *sec = queue.pop_back_val();

    // Follow ELF relocations to find referenced sections
    for (const InputSectionReloc &reloc : sec->getELFRelocations()) {
      if (!reloc.symbol)
        continue;

      if (auto *d = dyn_cast<Defined>(reloc.symbol)) {
        enqueue(getSectionForSymbol(d));
      }
      // ImportedSymbol doesn't contribute local sections
    }

    // PEF relocations use bytecode format - they typically reference
    // intra-section or import table, not useful for cross-section GC marking
  }
}

void lld::pef::markLive(std::vector<OutputSection *> &outputSections,
                        SymbolTable *symtab) {
  if (!config->gcSections)
    return;

  if (config->verbose)
    errorHandler().outs() << "\nPerforming garbage collection...\n";

  MarkLiveImpl(outputSections, symtab).run();

  // Report removed sections if requested
  if (config->printGcSections) {
    for (OutputSection *osec : outputSections) {
      for (InputSection *isec : osec->getInputSections()) {
        if (!isec->isLive()) {
          errorHandler().outs() << "removing unused section "
                                << isec->getName() << "\n";
        }
      }
    }
  }
}
