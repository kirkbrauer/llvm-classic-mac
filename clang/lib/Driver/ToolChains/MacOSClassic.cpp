//===--- MacOSClassic.cpp - Classic Mac OS ToolChain -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the Classic Mac OS (System 7-9) ToolChain.
// Classic Mac OS is NOT a Darwin-based system and predates Mac OS X.
//
//===----------------------------------------------------------------------===//

#include "MacOSClassic.h"
#include "CommonArgs.h"
#include "clang/Config/config.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/Options.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Support/Path.h"

using namespace clang::driver;
using namespace clang::driver::toolchains;
using namespace clang;
using namespace llvm::opt;

/// MacOSClassic ToolChain - Classic Mac OS (System 7, 8, and 9) toolchain.

/// Compute the sysroot path (following BareMetal pattern)
static std::string computeBaseSysRoot(const Driver &D, bool IncludeTriple) {
  if (!D.SysRoot.empty())
    return D.SysRoot;

  SmallString<128> SysRootDir(D.Dir);
  llvm::sys::path::append(SysRootDir, "..", "lib", "clang-runtimes");

  if (IncludeTriple)
    llvm::sys::path::append(SysRootDir, D.getTargetTriple());

  return std::string(SysRootDir);
}

MacOSClassic::MacOSClassic(const Driver &D, const llvm::Triple &Triple,
                           const ArgList &Args)
    : ToolChain(D, Triple, Args),
      SysRoot(computeBaseSysRoot(D, /*IncludeTriple=*/true)) {
  // Classic Mac OS is PowerPC only
  if (Triple.getArch() != llvm::Triple::ppc) {
    D.Diag(diag::err_drv_invalid_arch_for_classic_macos)
        << Triple.getArchName();
  }

  // Validate Classic Mac OS version
  VersionTuple Version;
  if (!Triple.getMacOSClassicVersion(Version)) {
    D.Diag(diag::err_drv_invalid_classic_macos_version)
        << Triple.getOSName();
  }

  // Warn if Mac OS 9 is used without G3+ processor
  if (Version.getMajor() == 9) {
    // Check if user explicitly specified a CPU
    if (Arg *A = Args.getLastArg(options::OPT_mcpu_EQ)) {
      StringRef CPU = A->getValue();
      // Check if CPU is pre-G3 (601, 603, 604, or generic "ppc")
      if (CPU == "601" || CPU == "602" || CPU == "603" || CPU == "603e" ||
          CPU == "603ev" || CPU == "604" || CPU == "604e" || CPU == "620" ||
          CPU == "ppc" || CPU == "powerpc") {
        D.Diag(diag::warn_drv_macos9_requires_g3) << CPU;
      }
    }
  }

  // Set up library paths for Classic Mac OS sysroot
  getProgramPaths().push_back(getDriver().Dir);

  SmallString<128> SysRootPath(computeSysRoot());
  if (!SysRootPath.empty()) {
    SmallString<128> LibPath(SysRootPath);
    llvm::sys::path::append(LibPath, "lib");
    getFilePaths().push_back(std::string(LibPath));
    getLibraryPaths().push_back(std::string(LibPath));
  }
}

std::string MacOSClassic::computeSysRoot() const {
  return SysRoot;
}

void MacOSClassic::AddClangSystemIncludeArgs(const ArgList &DriverArgs,
                                             ArgStringList &CC1Args) const {
  if (DriverArgs.hasArg(options::OPT_nostdinc))
    return;

  if (!DriverArgs.hasArg(options::OPT_nobuiltininc)) {
    SmallString<128> Dir(getDriver().ResourceDir);
    llvm::sys::path::append(Dir, "include");
    addSystemInclude(DriverArgs, CC1Args, Dir.str());
  }

  if (DriverArgs.hasArg(options::OPT_nostdlibinc))
    return;

  // Automatically include MacHeadersCompat.h before any other headers
  // This provides compatibility shims for Classic Mac OS Universal Interfaces
  // The header is installed in the clang resource directory
  SmallString<128> CompatHeader(getDriver().ResourceDir);
  llvm::sys::path::append(CompatHeader, "include", "MacHeadersCompat.h");

  // Add -include MacHeadersCompat.h to force include it
  CC1Args.push_back("-include");
  CC1Args.push_back(DriverArgs.MakeArgString(CompatHeader));

  const SmallString<128> SysRootPath(computeSysRoot());
  if (!SysRootPath.empty()) {
    // Add the include directory to search path
    SmallString<128> Dir(SysRootPath);
    llvm::sys::path::append(Dir, "include");
    addSystemInclude(DriverArgs, CC1Args, Dir.str());
  }
}

Tool *MacOSClassic::buildLinker() const {
  // For now, return nullptr - linker support will be added later
  // Classic Mac OS used different linking mechanisms than modern systems
  return nullptr;
}

Tool *MacOSClassic::buildAssembler() const {
  // For now, return nullptr - assembler support will be added later
  // Classic Mac OS assembler support can be added as needed
  return nullptr;
}
