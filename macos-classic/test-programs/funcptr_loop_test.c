// funcptr_loop_test.c - Simulates __cxa_finalize loop pattern
// Tests: Multiple function pointer calls in a while loop with array and counter
//
// This closely mimics what __cxa_finalize does:
// - static array of function pointers
// - static counter
// - while loop that decrements counter and calls handlers
//
// Expected:
//   1 long beep (start)
//   2 short beeps (handlers called in reverse order)
//   1 long beep (after loop)
//   3 very short beeps (success)
//   Total: 7 beeps

extern void SysBeep(short duration);
extern void Delay(long numTicks, long *finalTicks);
extern void ExitToShell(void) __attribute__((noreturn));

// Simulate atexit infrastructure
#define MAX_HANDLERS 4
static void (*handlers[MAX_HANDLERS])(void) = {0};
static int handler_count = 0;

// Track which handlers were called
static int handler1_called = 0;
static int handler2_called = 0;

// Handler functions
void handler1(void) {
    handler1_called = 1;
    SysBeep(10);       // Short beep
    Delay(30, 0);
}

void handler2(void) {
    handler2_called = 1;
    SysBeep(10);       // Short beep
    Delay(30, 0);
}

// Simulates atexit() - register a handler
int register_handler(void (*func)(void)) {
    if (handler_count >= MAX_HANDLERS)
        return -1;
    handlers[handler_count++] = func;
    return 0;
}

// Simulates __cxa_finalize() - run handlers in reverse order
void run_handlers(void) {
    while (handler_count > 0) {
        handler_count--;
        void (*handler)(void) = handlers[handler_count];
        handlers[handler_count] = 0;  // Clear slot
        if (handler) {
            handler();  // Indirect call through TVector
        }
    }
}

// Entry point
void __start(void) {
    // Beep: Start
    SysBeep(30);
    Delay(60, 0);

    // Register two handlers (like atexit does)
    register_handler(handler1);
    register_handler(handler2);

    // Run handlers (like __cxa_finalize does)
    // Should call handler2 first, then handler1 (LIFO order)
    run_handlers();

    // Beep: After loop completed
    SysBeep(30);
    Delay(60, 0);

    // Verify handlers were called
    if (handler1_called && handler2_called && handler_count == 0) {
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
