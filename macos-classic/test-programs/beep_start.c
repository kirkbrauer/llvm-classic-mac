//===-- beep_start.c - Entry Point for Split Test ------------*- C -*-===//
//
// This file contains the __start() entry point
// Will be linked with beep_main.c to test multi-file linking
//
//===----------------------------------------------------------------------===//

// Forward declare main (defined in beep_main.c)
int main(void);

// Forward declare ExitToShell
void ExitToShell(void) __attribute__((noreturn));

// Entry point - calls main() then exits
// The compiler emits TOC restore after calling main(), but the linker
// optimizes it away since main() is in the same PEF fragment.
void __start(void) {
    main();
    ExitToShell();
}
