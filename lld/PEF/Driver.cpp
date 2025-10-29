//===- Driver.cpp ---------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Driver.h"
#include "Config.h"

#include "lld/Common/Args.h"
#include "lld/Common/CommonLinkerContext.h"
#include "lld/Common/ErrorHandler.h"
#include "lld/Common/LLVM.h"
#include "lld/Common/Version.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
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

  // TODO: Phase 1.2 - Read input files
  // TODO: Phase 1.3 - Symbol resolution
  // TODO: Phase 1.4 - Section merging
  // TODO: Phase 1.5 - Relocations
  // TODO: Phase 1.6 - Write output

  warn("PEF linker not yet fully implemented - Phase 1 in progress");

  return true;
}

} // namespace lld::pef
