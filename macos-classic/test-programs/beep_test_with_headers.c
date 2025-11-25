//===-- beep_test_with_headers.c - Test With Real Mac OS Headers -*- C -*-===//
//
// Test program using real Mac OS headers (Sound.h, OSUtils.h)
// This tests if including headers causes any linking issues
//
// Expected behavior: Same as beep_test_with_main.c but using system headers
//===----------------------------------------------------------------------===//

#include <Sound.h>
#include <OSUtils.h>

// User's main function - called by __start
int main(void) {
    // Beep for 30 ticks (half second)
    SysBeep(30);

    // Delay for 60 ticks (1 second)
    Delay(60, NULL);

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
