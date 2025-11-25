// beep_atexit_exact_replica.c - EXACT single-file replica of multi-file linked binary
//
// This file contains ALL code from:
// 1. beep_atexit_simple.c (cleanup_handler, main) - FIRST in link order
// 2. macos_classic_cxx.c (runtime) - SECOND in link order
// 3. macos_classic_start.c (__start) - THIRD in link order
//
// Purpose: Create identical binary to multi-file version for byte-by-byte comparison
// Expected: 2 beeps (same as multi-file version should produce)

extern void SysBeep(short duration);
extern void Delay(long numTicks, long *finalTicks);
extern void ExitToShell(void) __attribute__((noreturn));

// Forward declarations
int main(int argc, char *argv[]);
int atexit(void (*func)(void));
void exit(int status) __attribute__((noreturn));

//=============================================================================
// FROM beep_atexit_simple.c (FIRST in link order)
//=============================================================================

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

    return 0;
}

//=============================================================================
// FROM macos_classic_cxx.c (SECOND in link order)
//=============================================================================

#define NULL ((void *)0)
#define MAX_ATEXIT_HANDLERS 32

// DSO handle - points to itself for identification
void *__dso_handle = &__dso_handle;

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

// __cxa_atexit: Register destructor (simplified - ignores arg/dso)
int __cxa_atexit(void (*func)(void *), void *arg, void *dso) {
    (void)arg;
    (void)dso;
    return atexit((void (*)(void))func);
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

// Static Initialization Guards (Simplified - no threading)
__attribute__((weak)) int __cxa_guard_acquire(int *guard) {
    if (*guard)
        return 0;
    *guard = 1;
    return 1;
}

__attribute__((weak)) void __cxa_guard_release(int *guard) {
    *guard = 1;
}

__attribute__((weak)) void __cxa_guard_abort(int *guard) {
    *guard = 0;
}

// Pure Virtual Handlers
__attribute__((weak)) void __cxa_pure_virtual(void) {
    while (1);  // Programming error - hang
}

__attribute__((weak)) void __cxa_deleted_virtual(void) {
    while (1);  // Programming error - hang
}

// Exception Frame Stubs
__attribute__((weak)) void __register_frame_info(const void *frame, void *obj) {
    (void)frame;
    (void)obj;
}

__attribute__((weak)) void *__deregister_frame_info(const void *frame) {
    (void)frame;
    return NULL;
}

// Exit function
void exit(int status) {
    (void)status;
    __cxa_finalize(__dso_handle);  // CRITICAL: uses __dso_handle, not NULL!
    ExitToShell();
    __builtin_unreachable();
}

//=============================================================================
// FROM macos_classic_start.c (THIRD in link order)
//=============================================================================

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
