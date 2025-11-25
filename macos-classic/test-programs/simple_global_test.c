//===-- simple_global_test.c - Test global variable access -----*- C -*-===//
//
// Simplified test to check if global variable references work correctly
//
//===----------------------------------------------------------------------===//

extern void ExitToShell(void) __attribute__((noreturn));

// Global variable in .data section
int test_value = 42;

void __start(void) {
    // Access global variable
    if (test_value == 42) {
        ExitToShell();
    }

    __builtin_unreachable();
}
