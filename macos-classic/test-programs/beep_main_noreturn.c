//===-- beep_main_noreturn.c - Main Function (No Return) -------*- C -*-===//
//
// This file contains the main() function that calls ExitToShell directly
// Testing if the issue is related to function returns
//
//===----------------------------------------------------------------------===//

// Forward declarations
void SysBeep(short duration);
void Delay(long numTicks, long *finalTicks);
void ExitToShell(void) __attribute__((noreturn));

void main(void) __attribute__((noreturn));

void main(void) {
    // Beep for 30 ticks (half second)
    SysBeep(30);

    // Delay for 60 ticks (1 second)
    Delay(60, 0);

    // Exit directly (don't return to __start)
    ExitToShell();
}
