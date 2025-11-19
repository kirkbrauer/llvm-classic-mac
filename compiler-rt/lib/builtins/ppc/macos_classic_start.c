//===-- macos_classic_start.c - PowerPC Classic Mac OS startup -*- C -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Startup code for PowerPC Classic Mac OS (PEF/CFM) applications.
//
// This provides the entry point called by the Code Fragment Manager (CFM)
// when a PEF application is launched. CFM handles:
// - TOC (Table of Contents) register (r2) initialization
// - Code and data relocation
// - Import library resolution
// - .data and .bss section initialization
//
// This startup code:
// 1. Calls main()
// 2. Exits to shell
//
// NOTE: C++ runtime support (atexit, __cxa_finalize) is provided by
// macos_classic_cxx.o when needed. This file provides minimal startup
// for both C and C++ programs.
//
//===----------------------------------------------------------------------===//

// External references
extern int main(int argc, char *argv[]);
extern void ExitToShell(void) __attribute__((noreturn));

//===----------------------------------------------------------------------===//
// Entry Point
//===----------------------------------------------------------------------===//

// Entry point called by Code Fragment Manager
// CFM has already initialized TOC (r2), performed relocations,
// and set up .data/.bss sections before calling this function.
void __start(void) {
  // Classic Mac applications don't have command-line arguments
  // Provide minimal argc/argv for compatibility with standard main()
  char *argv[2] = {"app", (char *)0};

  // Call the application's main function
  int result = main(1, argv);

  // Exit to Mac OS (never returns)
  // Note: Classic Mac OS doesn't use the return value from main()
  (void)result;
  ExitToShell();
  __builtin_unreachable();
}
