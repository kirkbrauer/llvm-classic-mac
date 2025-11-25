//===-- beep_test_with_main.c - Test Internal Function Calls ----*- C -*-===//
//
// Test program to validate function calls within the same file
// This has both main() and __start() to test internal function calling
//
// Expected behavior: __start() calls main(), which does the actual work
//===----------------------------------------------------------------------===//

// Direct function declarations (no headers needed)
void SysBeep(short duration);
void Delay(long numTicks, long *finalTicks);
void ExitToShell(void) __attribute__((noreturn));

// User's main function - called by __start
int main(void) {
    long finalTicks;

    // Beep for 30 ticks (half second)
    SysBeep(30);

    // Delay for 60 ticks (1 second)
    Delay(60, &finalTicks);

    // Return to __start
    return 0;
}

// Entry point - calls main() then exits
void __start(void) {
    // Call user's main function
    (void)main();

    // Exit to shell
    ExitToShell();
}
