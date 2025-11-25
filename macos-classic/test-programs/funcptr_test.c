// funcptr_test.c - Minimal function pointer test
// Tests: TVector indirect call + static variable access
//
// Expected: 2 long beeps + 3 short beeps, then clean exit
// If infinite beeps: function pointer call corrupts TOC
// If 1 long beep + crash: TVector dereference wrong
// If 2 long beeps + no success pattern: static variable access broken

extern void SysBeep(short duration);
extern void Delay(long numTicks, long *finalTicks);
extern void ExitToShell(void) __attribute__((noreturn));

// Static counter to verify data section access after indirect call
static int call_count = 0;

// Simple handler - increment counter and beep
void test_handler(void) {
    call_count++;      // Write to static variable
    SysBeep(10);       // Short beep (different from main)
    Delay(30, 0);      // Brief pause
}

// Entry point
void __start(void) {
    void (*handler)(void);

    // Beep 1: Before function pointer call (long beep)
    SysBeep(30);
    Delay(60, 0);

    // Store function pointer and call it
    handler = test_handler;
    handler();  // Indirect call through TVector

    // Beep 2: After function pointer call (proves TOC restored, long beep)
    SysBeep(30);
    Delay(60, 0);

    // Verify static variable was modified (proves data section access works)
    if (call_count == 1) {
        // Success pattern: 3 very short beeps
        SysBeep(5);
        Delay(10, 0);
        SysBeep(5);
        Delay(10, 0);
        SysBeep(5);
        Delay(30, 0);
    }

    ExitToShell();
}
