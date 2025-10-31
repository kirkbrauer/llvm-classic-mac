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
#include "clang/Driver/InputInfo.h"
#include "clang/Driver/Options.h"
#include "clang/Driver/Types.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"

using namespace clang::driver;
using namespace clang::driver::toolchains;
using namespace clang::driver::tools;
using namespace clang;
using namespace llvm::opt;

namespace tools {
namespace macosclassic {

/// Linker tool for Classic Mac OS (PEF/CFM format)
class LLVM_LIBRARY_VISIBILITY Linker : public Tool {
public:
  Linker(const ToolChain &TC) : Tool("macosclassic::Linker", "ld.lld", TC) {}

  bool hasIntegratedCPP() const override { return false; }
  bool isLinkJob() const override { return true; }

  void ConstructJob(Compilation &C, const JobAction &JA,
                    const InputInfo &Output, const InputInfoList &Inputs,
                    const ArgList &Args,
                    const char *LinkingOutput) const override;
};

void Linker::ConstructJob(Compilation &C, const JobAction &JA,
                          const InputInfo &Output,
                          const InputInfoList &Inputs, const ArgList &Args,
                          const char *LinkingOutput) const {
  ArgStringList CmdArgs;
  const auto &TC =
      static_cast<const toolchains::MacOSClassic &>(getToolChain());
  const Driver &D = TC.getDriver();

  // Set PEF flavor for the linker
  CmdArgs.push_back("-flavor");
  CmdArgs.push_back("pef");

  // Add entry point
  CmdArgs.push_back("-e");
  if (Args.hasArg(options::OPT_e))
    CmdArgs.push_back(Args.getLastArgValue(options::OPT_e).data());
  else
    CmdArgs.push_back("__start");

  // Add input object files
  AddLinkerInputs(TC, Inputs, Args, CmdArgs, JA);

  // Add runtime libraries (unless disabled with -nostdlib or -nodefaultlibs)
  if (!Args.hasArg(options::OPT_nostdlib, options::OPT_nodefaultlibs)) {
    // Add the compiler-rt builtins library object files directly
    // TODO: Once PEF linker supports .a archives, use AddRunTimeLibs() instead
    // This provides __start, qd globals, and C++ runtime support
    SmallString<128> RuntimePath(D.ResourceDir);
    llvm::sys::path::append(RuntimePath, "lib", "macosclassic");

    // Link runtime object files directly since PEF linker doesn't support archives yet
    const char *RuntimeFiles[] = {
      "macos_classic_start.o",
      "macos_classic_qd.o",
      "macos_classic_cxx.o"
    };

    for (const char *File : RuntimeFiles) {
      SmallString<128> FilePath(RuntimePath);
      llvm::sys::path::append(FilePath, File);
      if (llvm::sys::fs::exists(FilePath)) {
        CmdArgs.push_back(Args.MakeArgString(FilePath));
      }
    }

    // Add standard Mac OS libraries if requested
    // User can add -lInterfaceLib, -lMathLib, etc. via command line
  }

  // Add user-specified libraries
  Args.AddAllArgs(CmdArgs, options::OPT_L);
  TC.AddFilePathLibArgs(Args, CmdArgs);
  Args.addAllArgs(CmdArgs, {options::OPT_l, options::OPT_T_Group});

  // Output file
  CmdArgs.push_back("-o");
  CmdArgs.push_back(Output.getFilename());

  // Verbose mode for debugging
  if (Args.hasArg(options::OPT_v))
    CmdArgs.push_back("-v");

  // Get the linker path (lld with PEF support)
  const char *Exec = Args.MakeArgString(TC.GetLinkerPath());

  // Create and add the link command
  C.addCommand(std::make_unique<Command>(
      JA, *this, ResponseFileSupport::AtFileCurCP(), Exec, CmdArgs, Inputs,
      Output));
}

} // namespace macosclassic
} // namespace tools

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

std::string MacOSClassic::GetLinkerPath(bool *LinkerIsLLD) const {
  // Default to using lld for Mac OS Classic (PEF format support)
  if (LinkerIsLLD)
    *LinkerIsLLD = true;

  // Look for lld in the same directory as the compiler
  SmallString<128> LLDPath(getDriver().Dir);
  llvm::sys::path::append(LLDPath, "ld.lld");
  if (llvm::sys::fs::can_execute(LLDPath))
    return std::string(LLDPath);

  // Fall back to lld in PATH
  if (llvm::ErrorOr<std::string> LLDPathInPath =
          llvm::sys::findProgramByName("ld.lld"))
    return *LLDPathInPath;

  // Last resort: use the base class implementation
  return ToolChain::GetLinkerPath(LinkerIsLLD);
}

void MacOSClassic::addClangTargetOptions(
    const ArgList &DriverArgs, ArgStringList &CC1Args,
    Action::OffloadKind DeviceOffloadKind) const {
  // Classic Mac OS specific compiler options can be added here if needed
  // RTTI and exceptions are disabled by default via CalculateRTTIMode()
  // and the Clang.cpp driver code, respectively.
}

Tool *MacOSClassic::buildLinker() const {
  return new ::tools::macosclassic::Linker(*this);
}

Tool *MacOSClassic::buildAssembler() const {
  // For now, return nullptr - assembler support will be added later
  // Classic Mac OS assembler support can be added as needed
  return nullptr;
}
