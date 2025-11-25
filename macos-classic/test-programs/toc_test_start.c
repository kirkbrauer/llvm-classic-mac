//===-- toc_test_start.c - TOC Optimization Test Entry Point ---*- C -*-===//
//
// Test both same-fragment calls (should have TOC restore removed)
// and cross-fragment calls (should keep TOC restore)
//
//===----------------------------------------------------------------------===//

// Forward declare main (same fragment - should NOT need TOC restore)
int main(void);

// Forward declare external import (cross fragment - SHOULD need TOC restore)
extern void ExitToShell(void) __attribute__((noreturn));

// Entry point
void __start(void) {
    // Call to main() - same fragment, linker should remove TOC restore
    int result = main();

    // Call to ExitToShell() - import stub, linker should keep TOC restore
    ExitToShell();
}
