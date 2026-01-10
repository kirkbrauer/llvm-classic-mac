//===- Driver.cpp - Classic 68K linker driver -----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Driver.h"
#include "Config.h"
#include "ELFReader.h"
#include "Writer.h"
#include "lld/Common/CommonLinkerContext.h"
#include "lld/Common/Driver.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace llvm::opt;

namespace lld::classic68k {

// Define option IDs
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

// Define option info table
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

class Classic68KOptTable : public opt::GenericOptTable {
public:
  Classic68KOptTable() : GenericOptTable(OptionStrTable, OptionPrefixesTable, optInfo) {}
};

static void printHelp(const char *argv0) {
  Classic68KOptTable().printHelp(
      outs(), (Twine(argv0) + " [options] file...").str().c_str(),
      "Classic 68K linker", false);
}

static void printVersion() {
  outs() << "LLD Classic 68K Linker\n";
}

bool link(ArrayRef<const char *> args, raw_ostream &stdoutOS,
          raw_ostream &stderrOS, bool exitEarly, bool disableOutput) {
  // Set up linker context
  CommonLinkerContext ctx;

  // Parse command line options
  Classic68KOptTable table;
  unsigned missingIndex, missingCount;
  opt::InputArgList inputArgs =
      table.ParseArgs(args.slice(1), missingIndex, missingCount);

  if (missingCount) {
    stderrOS << "error: missing argument to " << inputArgs.getArgString(missingIndex)
             << "\n";
    return false;
  }

  // Handle help and version
  if (inputArgs.hasArg(OPT_help)) {
    printHelp(args[0]);
    return true;
  }

  if (inputArgs.hasArg(OPT_version)) {
    printVersion();
    return true;
  }

  // Create configuration
  Config c;
  config = &c;

  // Process options
  if (auto *arg = inputArgs.getLastArg(OPT_output))
    config->outputFile = arg->getValue();

  if (auto *arg = inputArgs.getLastArg(OPT_entry))
    config->entrySymbol = arg->getValue();

  if (auto *arg = inputArgs.getLastArg(OPT_segment_name))
    config->segmentName = arg->getValue();

  if (auto *arg = inputArgs.getLastArg(OPT_creator))
    config->creatorCode = arg->getValue();

  if (auto *arg = inputArgs.getLastArg(OPT_stack_size))
    StringRef(arg->getValue()).getAsInteger(0, config->stackSize);

  if (auto *arg = inputArgs.getLastArg(OPT_heap_size))
    StringRef(arg->getValue()).getAsInteger(0, config->heapSize);

  if (auto *arg = inputArgs.getLastArg(OPT_min_heap_size))
    StringRef(arg->getValue()).getAsInteger(0, config->minHeapSize);
  else
    config->minHeapSize = config->heapSize;

  config->verbose = inputArgs.hasArg(OPT_verbose);

  // Add search paths
  for (auto *arg : inputArgs.filtered(OPT_L))
    config->searchPaths.push_back(arg->getValue());

  // Collect input files
  for (auto *arg : inputArgs.filtered(OPT_INPUT))
    config->inputFiles.push_back(arg->getValue());

  // Handle -l options
  for (auto *arg : inputArgs.filtered(OPT_l)) {
    // TODO: Search for library in search paths
    config->inputFiles.push_back(std::string("lib") + arg->getValue() + ".a");
  }

  if (config->inputFiles.empty()) {
    stderrOS << "error: no input files\n";
    return false;
  }

  // Set default output file
  if (config->outputFile.empty()) {
    config->outputFile = "a.out";
  }

  // Load input files
  std::vector<std::unique_ptr<ELFReader>> readers;
  for (const auto &path : config->inputFiles) {
    auto reader = std::make_unique<ELFReader>();
    if (!reader->load(path)) {
      stderrOS << "error: " << reader->getError() << "\n";
      return false;
    }
    readers.push_back(std::move(reader));
  }

  // Create writer and link
  Writer writer;
  writer.setOutput(config->outputFile);
  for (auto &reader : readers)
    writer.addInput(reader.get());

  if (!writer.link()) {
    stderrOS << "error: " << writer.getError() << "\n";
    return false;
  }

  return true;
}

} // namespace lld::classic68k
