/*
 * Minimal test program for deep comparison
 *
 * This program has:
 * - No C++ runtime initialization
 * - No global constructors/destructors
 * - No main() function
 * - Direct entry point __start
 * - Single call to ExitToShell
 *
 * Purpose: Compare LLVM vs GCC PEF generation at each compilation stage
 */

// External import from InterfaceLib
void ExitToShell(void) __attribute__((noreturn));

// Entry point - CFM will call this via TVect
void __start(void) {
    ExitToShell();
}
