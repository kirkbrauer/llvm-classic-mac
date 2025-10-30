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
#include "Relocations.h"
#include "SymbolTable.h"
#include "Writer.h"

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

namespace lld {
namespace pef {

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
  // Default to "main" for MPW-style command-line tools
  // Classic Mac OS applications use initialization routines specified in the fragment,
  // not a Unix-style __start entry point
  config->entry = args.getLastArgValue(OPT_e, "main");

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

  // PEF shared libraries (Phase 2)
  for (const Arg *arg : args.filtered(OPT_l))
    config->libraries.push_back(arg->getValue());

  for (const Arg *arg : args.filtered(OPT_weak_l))
    config->weakLibraries.push_back(arg->getValue());

  // Input files (positional arguments)
  for (const Arg *arg : args.filtered(OPT_INPUT))
    config->inputFiles.push_back(arg->getValue());
}

// Search for a library file in library search paths
// Returns the full path if found, empty string otherwise
static std::string searchLibrary(StringRef name) {
  // Try variations of the library name:
  // 1. Direct path (if absolute or relative with path separator)
  // 2. With "lib" prefix (libName)
  // 3. Without extension (Name, libName)
  // 4. With .a extension (Name.a, libName.a)
  // 5. Without prefix/extension (Name)

  SmallVector<std::string, 8> candidates;

  // If name contains path separator, treat as direct path
  if (name.contains('/') || name.contains('\\')) {
    candidates.push_back(name.str());
  } else {
    // Try different naming conventions
    candidates.push_back(name.str());                      // InterfaceLib
    candidates.push_back(("lib" + name).str());            // libInterfaceLib
    candidates.push_back((name + ".a").str());             // InterfaceLib.a
    candidates.push_back(("lib" + name + ".a").str());     // libInterfaceLib.a
    candidates.push_back((name + ".pef").str());           // InterfaceLib.pef
  }

  // Build search paths list
  SmallVector<StringRef, 8> searchDirs;

  // 1. Add -L paths (highest priority)
  for (const std::string &libPath : config->libraryPaths) {
    searchDirs.push_back(libPath);
  }

  // 2. Add sysroot library path (standard location for Classic Mac OS SDK)
  //    Relative to lld binary: ../lib/clang-runtimes/powerpc-apple-macos-9/lib
  //    This matches the BareMetal pattern used by the clang driver
  searchDirs.push_back("../lib/clang-runtimes/powerpc-apple-macos-9/lib");
  searchDirs.push_back("lib/clang-runtimes/powerpc-apple-macos-9/lib");

  // 3. Add test library path for development/testing (minimal set of libs)
  searchDirs.push_back("../lld/test/PEF/Inputs/lib");
  searchDirs.push_back("lld/test/PEF/Inputs/lib");

  // 4. Fallback: external Retro68 location if available
  searchDirs.push_back("../Retro68/InterfacesAndLibraries/Libraries/SharedLibraries");
  searchDirs.push_back("/Users/kirk/repos/toolchain-macos9/Retro68/InterfacesAndLibraries/Libraries/SharedLibraries");

  // Try each candidate in each search directory
  for (StringRef searchDir : searchDirs) {
    for (const std::string &candidate : candidates) {
      SmallString<256> fullPath(searchDir);
      path::append(fullPath, candidate);

      if (fs::exists(fullPath)) {
        if (config->verbose) {
          errorHandler().outs() << "Found library: " << fullPath << "\n";
        }
        return fullPath.str().str();
      }
    }
  }

  return ""; // Not found
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

  // Phase 2.1 - Load PEF shared libraries
  std::vector<SharedLibraryFile *> importLibs;

  // Load regular shared libraries (-l)
  for (const std::string &libName : config->libraries) {
    std::string libPath = searchLibrary(libName);
    if (libPath.empty()) {
      error("library not found: " + libName);
      continue;
    }

    if (auto mbref = readFile(libPath)) {
      if (SharedLibraryFile *lib = createSharedLibraryFile(*mbref, false)) {
        importLibs.push_back(lib);
        files.push_back(lib);
        if (config->verbose) {
          errorHandler().outs() << "Loaded shared library: " << libPath << "\n";
        }
      }
    }
  }

  // Load weak shared libraries (--weak-l)
  for (const std::string &libName : config->weakLibraries) {
    std::string libPath = searchLibrary(libName);
    if (libPath.empty()) {
      // Weak libraries are optional, just warn
      if (config->verbose) {
        errorHandler().outs() << "Warning: weak library not found: " << libName << "\n";
      }
      continue;
    }

    if (auto mbref = readFile(libPath)) {
      if (SharedLibraryFile *lib = createSharedLibraryFile(*mbref, true)) {
        importLibs.push_back(lib);
        files.push_back(lib);
        if (config->verbose) {
          errorHandler().outs() << "Loaded weak shared library: " << libPath << "\n";
        }
      }
    }
  }

  // Phase 1.3 - Symbol resolution (already done during parsing)
  // Phase 2.2 - Resolve undefined symbols against import libraries
  auto undefinedSymbols = symtab->getUndefinedSymbols();

  if (!undefinedSymbols.empty() && !importLibs.empty()) {
    if (config->verbose) {
      errorHandler().outs() << "\nResolving " << undefinedSymbols.size()
                           << " undefined symbol(s) against import libraries...\n";
    }

    // Try to resolve each undefined symbol
    for (auto *undef : undefinedSymbols) {
      StringRef symName = undef->getName();
      bool resolved = false;

      // Search all import libraries for this symbol
      for (SharedLibraryFile *lib : importLibs) {
        Symbol *exportedSym = lib->findExport(symName);
        if (exportedSym) {
          // Found the symbol in this library - create an imported symbol
          // Use the symbol class from the export, not from the undefined symbol
          symtab->addImported(symName, lib, lib->getLastSymbolClass(),
                             lib->isWeakImport());
          resolved = true;
          break;
        }
      }

      if (!resolved && config->verbose) {
        errorHandler().outs() << "  Symbol " << symName
                             << " not found in any import library\n";
      }
    }
  }

  // Re-check for undefined symbols after import resolution
  undefinedSymbols = symtab->getUndefinedSymbols();
  if (!undefinedSymbols.empty() && !config->allowUndefined) {
    for (auto *undef : undefinedSymbols) {
      error("undefined symbol: " + undef->getName());
    }
  }

  // Report symbol table statistics
  auto definedSymbols = symtab->getDefinedSymbols();
  auto importedSymbols = symtab->getImportedSymbols();

  if (config->verbose) {
    errorHandler().outs() << "\nSymbol Table Summary:\n";
    errorHandler().outs() << "  Defined symbols: " << definedSymbols.size()
                         << "\n";
    errorHandler().outs() << "  Imported symbols: " << importedSymbols.size()
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

  // Phase 1.5 - Process relocations
  // Scan relocations to determine symbol dependencies (important for Phase 2)
  for (OutputSection *osec : outputSections) {
    for (InputSection *isec : osec->getInputSections()) {
      scanRelocations(isec);
    }
  }

  // Apply relocations to fix up addresses
  if (config->verbose && !outputSections.empty()) {
    errorHandler().outs() << "\nProcessing relocations...\n";
  }

  for (OutputSection *osec : outputSections) {
    for (InputSection *isec : osec->getInputSections()) {
      processRelocations(isec);
    }
  }

  // Phase 1.6 - Write output
  if (errorCount() == 0) {
    writeResult(outputSections);
  }

  return errorCount() == 0;
}

} // namespace pef
} // namespace lld
