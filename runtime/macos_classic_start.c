// Mac OS Classic C Runtime Startup
// This file provides the __start entry point that wraps user's main() function
// and ensures proper application termination via ExitToShell()

// Mac OS Toolbox function prototypes
void ExitToShell(void) __attribute__((noreturn));

// User's main function prototype
// We support both void main(void) and int main(void)
extern int main(void);

// Entry point - called by CFM when application launches
// This function never returns - it calls ExitToShell which terminates the app
void __start(void) __attribute__((noreturn));

void __start(void) {
    // Call user's main function
    // We ignore the return value since we're going to call ExitToShell anyway
    (void)main();

    // Properly terminate the application
    // ExitToShell never returns - it exits to the Mac OS shell/Finder
    ExitToShell();

    // We should never reach here, but add an infinite loop just in case
    while (1) {
        // Spin forever if ExitToShell somehow returns
    }
}
