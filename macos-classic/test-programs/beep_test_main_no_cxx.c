//===-- beep_test_main_no_cxx.c - Simple Test Without C++ Runtime -*-C-*-===//
//
// Minimal test program that directly uses __start() entry point
// This bypasses the C++ runtime (macos_classic_cxx.o) to isolate issues
//
// This should behave identically to beep_test.c (which works)
//===----------------------------------------------------------------------===//

// Direct function declarations (no headers needed)
void SysBeep(short duration);
void Delay(long numTicks, long *finalTicks);
void ExitToShell(void) __attribute__((noreturn));

void __start(void) {
    long finalTicks;

    // Beep for 30 ticks (half second)
    SysBeep(30);

    // Delay for 60 ticks (1 second)
    Delay(60, &finalTicks);

    // Exit to shell
    ExitToShell();
}
