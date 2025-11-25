//===-- beep_runtime_test.c - Test with C++ Runtime -----------*- C -*-===//
//
// Test program that uses the C++ runtime (macos_classic_start.o + macos_classic_cxx.o)
// Entry flow: __start() -> main() -> exit() -> __cxa_finalize() -> ExitToShell()
//
//===----------------------------------------------------------------------===//

// Toolbox function declarations
void SysBeep(short duration);
void Delay(long numTicks, long *finalTicks);

// main() function - called by __start from macos_classic_start.o
// Returns to __start which calls exit() from macos_classic_cxx.o
int main(int argc, char **argv) {
    // Beep for 30 ticks (half second)
    SysBeep(30);

    // Delay for 60 ticks (1 second)
    Delay(60, 0);

    // Return to __start
    // __start will call exit(0) which calls __cxa_finalize() then ExitToShell()
    return 0;
}
