// beep_atexit_simple.c - Minimal atexit() test
//
// Tests basic atexit functionality with a simple function
// Expected: 2 beeps total (main + atexit handler)
//
// 1. Long beep (30 ticks) - Main function
// 2. Short beep (10 ticks) - atexit handler
//
// If we get an infinite loop of short beeps, the problem is in atexit infrastructure

extern void SysBeep(short duration);
extern void Delay(long numTicks, long *finalTicks);
extern int atexit(void (*func)(void));

void cleanup_handler(void) {
    SysBeep(10);  // Short beep
    Delay(60, 0); // 1 second pause
}

int main(int argc, char **argv) {
    // Register cleanup handler
    atexit(cleanup_handler);

    // Main beep
    SysBeep(30);  // Long beep
    Delay(60, 0); // 1 second pause

    return 0;  // Should call exit() which runs atexit handlers
}
