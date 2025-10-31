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
// This startup code only needs to:
// 1. Call global C++ constructors (if any)
// 2. Call main()
// 3. Call global C++ destructors (if any)
// 4. Return to CFM
//
//===----------------------------------------------------------------------===//

// External references
extern int main(int argc, char *argv[]);
extern void __cxa_finalize(void *dso);
extern int atexit(void (*func)(void));

// DSO handle for this executable - used by C++ runtime
extern void *__dso_handle;

//===----------------------------------------------------------------------===//
// Entry Point
//===----------------------------------------------------------------------===//

// Entry point called by Code Fragment Manager
// CFM has already initialized TOC (r2), performed relocations,
// and set up .data/.bss sections before calling this function.
void __start(void) {
  // Register global destructor handler to run on exit
  // This ensures C++ global object destructors are called
  atexit((void (*)(void))__cxa_finalize);

  // Classic Mac applications don't have command-line arguments
  // Provide minimal argc/argv for compatibility with standard main()
  char *argv[2] = {"app", (char *)0};

  // Call the application's main function
  int result = main(1, argv);

  // Call all registered destructors before exiting
  // This runs C++ global object destructors and atexit handlers
  __cxa_finalize(__dso_handle);

  // Return to Code Fragment Manager
  // CFM will handle final cleanup and return to Mac OS
  // Note: Classic Mac OS doesn't use the return value
  (void)result;
}
