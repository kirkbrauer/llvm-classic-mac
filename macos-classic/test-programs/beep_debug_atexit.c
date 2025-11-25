// beep_debug_atexit.c - Debug atexit execution flow
//
// Expected beep sequence:
// 1. LONG beep (30 ticks) - Start of main()
// 2. MEDIUM beep (20 ticks) - After atexit() registration
// 3. SHORT beep (10 ticks) - Inside cleanup_handler()
// 4. Exit cleanly
//
// If we hear: LONG, MEDIUM, then infinite SHORT beeps
//   -> The handler is being called repeatedly (loop bug)
//
// If we hear: LONG, MEDIUM, SHORT, SHORT, SHORT...
//   -> The handler runs but exit() is not working
//
// If we hear: LONG, then hang
//   -> Problem in atexit() registration itself

extern void SysBeep(short duration);
extern void Delay(long numTicks, long *finalTicks);
extern int atexit(void (*func)(void));

void cleanup_handler(void) {
    SysBeep(10);   // SHORT beep - 10 ticks
    Delay(30, 0);  // Brief pause
}

int main(int argc, char **argv) {
    // Mark start of main
    SysBeep(30);   // LONG beep - 30 ticks
    Delay(60, 0);  // 1 second pause

    // Register handler
    atexit(cleanup_handler);

    // Mark successful registration
    SysBeep(20);   // MEDIUM beep - 20 ticks
    Delay(60, 0);  // 1 second pause

    return 0;      // Triggers exit() -> __cxa_finalize() -> cleanup_handler()
}
