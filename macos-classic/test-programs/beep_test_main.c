//===-- beep_test_main.c - Mac OS 9 Test with C Runtime -------*- C -*-===//
//
// Test program for Mac OS 9 LLVM toolchain with C runtime support
//
// This program demonstrates:
// 1. Using standard main() function instead of __start()
// 2. Including real Mac OS headers (Sound.h, OSUtils.h)
// 3. Using return 0; to exit (runtime calls ExitToShell)
// 4. Pascal string support (enabled by default)
//
//===----------------------------------------------------------------------===//

#include <Sound.h>
#include <OSUtils.h>

int main(void) {
    // Beep for 30 ticks (half second)
    SysBeep(30);

    // Delay for 60 ticks (1 second)
    Delay(60, NULL);

    // Return to __start, which calls ExitToShell()
    return 0;
}
