//===- Driver.cpp ---------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Driver.h"
#include "Config.h"
#include "InputFiles.h"
#include "OutputSection.h"
#include "SymbolTable.h"

#include "lld/Common/Args.h"
#include "lld/Common/CommonLinkerContext.h"
#include "lld/Common/ErrorHandler.h"
#include "lld/Common/LLVM.h"
#include "lld/Common/Version.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/BinaryFormat/PEF.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

using namespace llvm;
using namespace llvm::opt;
using namespace llvm::sys;
using namespace lld;
using namespace lld::pef;

namespace lld::pef {

// Create enum with OPT_xxx values for each option in Options.td
enum {
  OPT_INVALID = 0,
#define OPTION(...) LLVM_MAKE_OPT_ID(__VA_ARGS__),
#include "Options.inc"
#undef OPTION
};

#define OPTTABLE_STR_TABLE_CODE
#include "Options.inc"
#undef OPTTABLE_STR_TABLE_CODE

#define OPTTABLE_PREFIXES_TABLE_CODE
#include "Options.inc"
#undef OPTTABLE_PREFIXES_TABLE_CODE

// Create table mapping all options defined in Options.td
static constexpr opt::OptTable::Info optInfo[] = {
#define OPTION(PREFIX, NAME, ID, KIND, GROUP, ALIAS, ALIASARGS, FLAGS,         \
               VISIBILITY, PARAM, HELPTEXT, HELPTEXTSFORVARIANTS, METAVAR,     \
               VALUES)                                                         \
  {PREFIX,                                                                     \
   NAME,                                                                       \
   HELPTEXT,                                                                   \
   HELPTEXTSFORVARIANTS,                                                       \
   METAVAR,                                                                    \
   OPT_##ID,                                                                   \
   opt::Option::KIND##Class,                                                   \
   PARAM,                                                                      \
   FLAGS,                                                                      \
   VISIBILITY,                                                                 \
   OPT_##GROUP,                                                                \
   OPT_##ALIAS,                                                                \
   ALIASARGS,                                                                  \
   VALUES},
#include "Options.inc"
#undef OPTION
};

PEFOptTable::PEFOptTable() : GenericOptTable(OptionStrTable, OptionPrefixesTable, optInfo) {}

InputArgList PEFOptTable::parse(CommonLinkerContext &ctx,
                                ArrayRef<const char *> argv) {
  // Parse options
  unsigned missingIndex;
  unsigned missingCount;
  InputArgList args = ParseArgs(argv.slice(1), missingIndex, missingCount);

  if (missingCount)
    error(Twine(args.getArgString(missingIndex)) + ": missing argument");

  // Handle unknown arguments
  for (const Arg *arg : args.filtered(OPT_UNKNOWN)) {
    error("unknown argument '" + arg->getAsString(args) + "'");
  }

  return args;
}

static void parseArgs(CommonLinkerContext &ctx, const InputArgList &args) {
  config = make<Config>();

  // Output file
  config->outputFile = args.getLastArgValue(OPT_o, "a.out");

  // Entry point
  config->entry = args.getLastArgValue(OPT_e, "_main");

  // Base addresses
  if (auto *arg = args.getLastArg(OPT_base_code)) {
    StringRef val = arg->getValue();
    if (val.getAsInteger(0, config->baseCode))
      error("--base-code: invalid value: " + val);
  }

  if (auto *arg = args.getLastArg(OPT_base_data)) {
    StringRef val = arg->getValue();
    if (val.getAsInteger(0, config->baseData))
      error("--base-data: invalid value: " + val);
  }

  // Verbose
  config->verbose = args.hasArg(OPT_verbose);

  // Allow undefined
  config->allowUndefined = args.hasArg(OPT_allow_undefined);

  // Library search paths (Phase 2)
  for (const Arg *arg : args.filtered(OPT_L))
    config->libraryPaths.push_back(arg->getValue());

  // Libraries (Phase 2)
  for (const Arg *arg : args.filtered(OPT_l))
    config->libraries.push_back(arg->getValue());

  for (const Arg *arg : args.filtered(OPT_weak_l))
    config->weakLibraries.push_back(arg->getValue());

  // Input files (positional arguments)
  for (const Arg *arg : args.filtered(OPT_INPUT))
    config->inputFiles.push_back(arg->getValue());
}

bool link(ArrayRef<const char *> argsArr, llvm::raw_ostream &stdoutOS,
          llvm::raw_ostream &stderrOS, bool exitEarly, bool disableOutput) {
  // This driver-specific context will be freed later by unsafeLldMain().
  auto *context = new CommonLinkerContext;

  context->e.initialize(stdoutOS, stderrOS, exitEarly, disableOutput);
  context->e.cleanupCallback = []() {
    config = nullptr;
  };

  context->e.logName = args::getFilenameWithoutExe(argsArr[0]);
  context->e.errorLimitExceededMsg =
      "too many errors emitted, stopping now (use "
      "--error-limit=0 to see all errors)";

  config = make<Config>();
  symtab = make<SymbolTable>();

  PEFOptTable parser;
  InputArgList args = parser.parse(*context, argsArr);

  // Handle --help
  if (args.hasArg(OPT_help)) {
    parser.printHelp(errorHandler().outs(),
                     (std::string(argsArr[0]) + " [options] <inputs>").c_str(),
                     "LLD PEF Linker", false);
    return true;
  }

  // Handle --version
  if (args.hasArg(OPT_version)) {
    errorHandler().outs() << getLLDVersion() << "\n";
    return true;
  }

  // Parse arguments
  parseArgs(*context, args);

  if (config->inputFiles.empty()) {
    error("no input files");
    return false;
  }

  if (config->verbose) {
    errorHandler().outs() << "LLD PEF Linker\n";
    errorHandler().outs() << "Output: " << config->outputFile << "\n";
    errorHandler().outs() << "Entry: " << config->entry << "\n";
    errorHandler().outs() << "Input files:\n";
    for (StringRef file : config->inputFiles)
      errorHandler().outs() << "  " << file << "\n";
  }

  // Phase 1.2 - Read input files
  std::vector<InputFile *> files;
  for (StringRef path : config->inputFiles) {
    if (auto mbref = readFile(path)) {
      if (InputFile *file = createObjectFile(*mbref)) {
        files.push_back(file);
      }
    }
  }

  if (files.empty()) {
    error("no valid input files");
    return false;
  }

  if (config->verbose) {
    errorHandler().outs() << "Successfully loaded " << files.size()
                         << " input file(s)\n";
  }

  // Phase 1.3 - Symbol resolution (already done during parsing)
  // Check for undefined symbols
  auto undefinedSymbols = symtab->getUndefinedSymbols();
  if (!undefinedSymbols.empty() && !config->allowUndefined) {
    for (auto *undef : undefinedSymbols) {
      error("undefined symbol: " + undef->getName());
    }
  }

  // Report symbol table statistics
  auto definedSymbols = symtab->getDefinedSymbols();
  if (config->verbose) {
    errorHandler().outs() << "\nSymbol Table Summary:\n";
    errorHandler().outs() << "  Defined symbols: " << definedSymbols.size()
                         << "\n";
    errorHandler().outs() << "  Undefined symbols: " << undefinedSymbols.size()
                         << "\n";

    if (!config->entry.empty()) {
      Symbol *entrySym = symtab->find(config->entry);
      if (!entrySym) {
        error("entry point symbol not found: " + config->entry);
      } else if (!entrySym->isDefined()) {
        error("entry point symbol is undefined: " + config->entry);
      } else {
        errorHandler().outs() << "  Entry point: " << config->entry << "\n";
      }
    }
  }

  // Phase 1.4 - Section merging and layout
  std::vector<OutputSection *> outputSections;

  // Create output sections for each section kind
  OutputSection *textSec = make<OutputSection>(".text", PEF::kPEFCodeSection);
  OutputSection *dataSec = make<OutputSection>(".data", PEF::kPEFUnpackedDataSection);
  OutputSection *rodataSec = make<OutputSection>(".rodata", PEF::kPEFConstantSection);

  outputSections.push_back(textSec);
  outputSections.push_back(dataSec);
  outputSections.push_back(rodataSec);

  // Collect input sections into output sections
  for (InputFile *file : files) {
    if (auto *obj = dyn_cast<ObjFile>(file)) {
      for (InputSection *isec : obj->getInputSections()) {
        switch (isec->getKind()) {
        case PEF::kPEFCodeSection:
        case PEF::kPEFExecutableDataSection:
          textSec->addInputSection(isec);
          break;
        case PEF::kPEFUnpackedDataSection:
        case PEF::kPEFPatternDataSection:
          dataSec->addInputSection(isec);
          break;
        case PEF::kPEFConstantSection:
          rodataSec->addInputSection(isec);
          break;
        default:
          // Skip unknown section kinds
          break;
        }
      }
    }
  }

  // Assign virtual addresses to output sections
  uint64_t addr = config->baseCode;
  for (OutputSection *osec : outputSections) {
    if (osec->getInputSections().empty())
      continue;

    // Align to section alignment
    addr = alignTo(addr, osec->getAlignment());
    osec->setVirtualAddress(addr);

    // Finalize layout (assigns addresses to input sections)
    osec->finalizeLayout();

    addr += osec->getSize();
  }

  // Update symbol virtual addresses based on their section assignments
  for (Defined *sym : definedSymbols) {
    int16_t secIdx = sym->getSectionIndex();
    if (secIdx < 0)
      continue; // Absolute or undefined

    // Find the input section containing this symbol
    for (OutputSection *osec : outputSections) {
      for (InputSection *isec : osec->getInputSections()) {
        if (isec->getIndex() == static_cast<unsigned>(secIdx) &&
            isec->getFile() == sym->getFile()) {
          uint64_t symAddr = isec->getVirtualAddress() + sym->getValue();
          sym->setVirtualAddress(symAddr);
          break;
        }
      }
    }
  }

  if (config->verbose) {
    errorHandler().outs() << "\nMemory Layout:\n";
    for (OutputSection *osec : outputSections) {
      if (osec->getInputSections().empty())
        continue;
      errorHandler().outs() << "  " << osec->getName()
                           << " @ 0x" << utohexstr(osec->getVirtualAddress())
                           << " size=0x" << utohexstr(osec->getSize()) << "\n";
    }
  }

  // TODO: Phase 1.5 - Relocations
  // TODO: Phase 1.6 - Write output

  warn("PEF linker not yet fully implemented - Phase 1 in progress");

  return errorCount() == 0;
}

} // namespace lld::pef
