//===-- beep_test_simple.c - Simplified beep test without C runtime -*-===//
//
// Simplified version of beep_test_main WITHOUT C runtime support
// Uses __start() directly instead of main()
// No atexit, no __cxa_finalize, no C++ runtime complexity
//
// This test helps isolate whether crashes are from:
// - C runtime complexity/bugs OR
// - Core code generation issues
//
//===----------------------------------------------------------------------===//

// External Mac OS Toolbox functions
extern void SysBeep(short duration);
extern void Delay(unsigned long ticks, unsigned long *finalTicks);
extern void ExitToShell(void) __attribute__((noreturn));

void __start(void) {
    // Beep for 30 ticks (half second)
    SysBeep(30);

    // Delay for 60 ticks (1 second)
    Delay(60, 0L);

    // Exit cleanly
    ExitToShell();

    __builtin_unreachable();
}
