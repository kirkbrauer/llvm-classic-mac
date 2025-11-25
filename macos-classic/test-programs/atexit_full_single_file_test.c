// atexit_full_single_file_test.c - Full runtime + start + test in single file
//
// This combines ALL runtime components with the test code:
// - macos_classic_cxx.c (atexit, __cxa_finalize, exit)
// - macos_classic_start.c (__start entry point calling main)
// - beep_atexit_simple.c test code (main function)
//
// Expected: 2 beeps total (main + atexit handler), then clean exit
// This mirrors exactly what the multi-file linked version should do.

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
// Start Entry Point (from macos_classic_start.c)
//===----------------------------------------------------------------------===//

// Forward declaration of main
int main(int argc, char *argv[]);

// Entry point called by Code Fragment Manager
void __start(void) {
  // Classic Mac applications don't have command-line arguments
  // Provide minimal argc/argv for compatibility with standard main()
  char *argv[2] = {"app", (char *)0};

  // Call the application's main function
  int result = main(1, argv);

  // Exit via C runtime (runs atexit handlers)
  exit(result);
}

//===----------------------------------------------------------------------===//
// Test Code (from beep_atexit_simple.c)
//===----------------------------------------------------------------------===//

void cleanup_handler(void) {
    SysBeep(10);  // Short beep
    Delay(60, 0); // 1 second pause
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    // Register cleanup handler
    atexit(cleanup_handler);

    // Main beep
    SysBeep(30);  // Long beep
    Delay(60, 0); // 1 second pause

    return 0;  // Should call exit() which runs atexit handlers
}
