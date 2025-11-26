//===-- time_test.c - Intermediate Mac OS 9 Test -------*- C -*-===//
//
// Test program for Mac OS 9 LLVM toolchain
//
// This program demonstrates:
// 1. TickCount() API - system timing
// 2. GetDateTime() API - current date/time with pointer parameter
// 3. Return value handling and pointer parameters
// 4. Arithmetic operations (subtraction, modulo)
// 5. Conditional logic and variable iteration loops
// 6. Real Mac OS header inclusion
//
// Expected behavior:
// - Phase 1: Entry beep
// - Phase 2: 1 second pause, then 2 short success beeps (TickCount works)
// - Phase 3: Variable number of beeps (0-9) based on current time
// - Phase 4: Long completion beep
//
//===----------------------------------------------------------------------===//

#include <Sound.h>
#include <OSUtils.h>

int main(void) {
    unsigned long start_tick, end_tick, elapsed;
    unsigned long current_time;
    int beep_count, i;

    // Phase 1: Entry signal
    SysBeep(20);
    Delay(30, 0);

    // Phase 2: TickCount timing test
    // Measure elapsed time during a 1-second delay
    start_tick = TickCount();
    Delay(60, 0);  // Wait 1 second (60 ticks)
    end_tick = TickCount();
    elapsed = end_tick - start_tick;

    // Validate timing (60 +/- 10 ticks)
    if (elapsed >= 50 && elapsed <= 70) {
        // Success: 2 short beeps
        SysBeep(10);
        Delay(20, 0);
        SysBeep(10);
        Delay(20, 0);
    } else {
        // Error: 5 rapid beeps
        for (i = 0; i < 5; i++) {
            SysBeep(5);
            Delay(10, 0);
        }
    }

    // Phase 3: GetDateTime variable pattern
    // Get current time and beep N times (where N = last digit of time)
    GetDateTime(&current_time);
    beep_count = (int)(current_time % 10);  // Last digit 0-9

    Delay(30, 0);
    for (i = 0; i < beep_count; i++) {
        SysBeep(8);
        Delay(15, 0);
    }

    // Phase 4: Completion signal
    Delay(30, 0);
    SysBeep(40);  // Long beep
    Delay(60, 0);

    return 0;
}
