//===-- beep_start_c.c - Entry Point for Split Test (C version) --*- C -*-===//
//
// This file contains the __start() entry point in C (no inline assembly)
// Will be linked with beep_main_fixed.c to test TOC optimization
//
//===----------------------------------------------------------------------===//

// Forward declare main (defined in beep_main_fixed.c)
int main(void);

// Forward declare ExitToShell
void ExitToShell(void) __attribute__((noreturn));

// Entry point - calls main() then exits
// The compiler will add TOC restore after calling main(), but the linker
// should optimize it away since main() is in the same fragment.
void __start(void) {
    main();
    ExitToShell();
}
