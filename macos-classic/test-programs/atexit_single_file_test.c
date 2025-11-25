// atexit_single_file_test.c - Combined runtime + atexit test in single file
//
// This combines the C++ runtime atexit implementation with the test code
// to isolate whether linking multiple files causes issues.
//
// Expected: 2 beeps total (main + atexit handler), then clean exit
// If infinite beeps: problem is in the runtime logic itself, not multi-file linking

extern void SysBeep(short duration);
extern void Delay(long numTicks, long *finalTicks);
extern void ExitToShell(void) __attribute__((noreturn));

//===----------------------------------------------------------------------===//
// Atexit Implementation (from macos_classic_cxx.c)
//===----------------------------------------------------------------------===//

#define NULL ((void *)0)
#define MAX_ATEXIT_HANDLERS 32

// Simple function pointer array - stores TVector addresses directly
static void (*atexit_handlers[MAX_ATEXIT_HANDLERS])(void) = {0};
static int atexit_count = 0;

// Standard C atexit - register function to call at exit
int atexit(void (*func)(void)) {
  if (func == NULL || atexit_count >= MAX_ATEXIT_HANDLERS)
    return -1;

  atexit_handlers[atexit_count++] = func;
  return 0;
}

// __cxa_finalize: Run registered handlers in reverse order
void __cxa_finalize(void *dso) {
  (void)dso;

  // Run handlers in reverse order (LIFO)
  while (atexit_count > 0) {
    atexit_count--;
    void (*handler)(void) = atexit_handlers[atexit_count];
    atexit_handlers[atexit_count] = NULL;
    if (handler) {
      handler();  // Call through function pointer (TVector)
    }
  }
}

// exit function
void exit(int status) {
  (void)status;
  __cxa_finalize(NULL);
  ExitToShell();
  __builtin_unreachable();
}

//===----------------------------------------------------------------------===//
// Test Code (from beep_atexit_simple.c)
//===----------------------------------------------------------------------===//

void cleanup_handler(void) {
    SysBeep(10);  // Short beep
    Delay(60, 0); // 1 second pause
}

// Entry point - using __start to avoid needing the full runtime
void __start(void) {
    // Register cleanup handler
    atexit(cleanup_handler);

    // Main beep
    SysBeep(30);  // Long beep
    Delay(60, 0); // 1 second pause

    // Call exit which should run atexit handlers
    exit(0);
}
