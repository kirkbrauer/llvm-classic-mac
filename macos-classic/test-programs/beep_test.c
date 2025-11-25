// Multi-function import test: SysBeep + Delay + ExitToShell
// This test validates:
// - Multiple function imports (3 symbols from InterfaceLib)
// - Parameter passing (single param, dual param with pointer)
// - Local variable allocation on stack
// - Import stub generation for multiple functions

void SysBeep(short duration);
void Delay(long numTicks, long *finalTicks);
void ExitToShell(void);

void __start(void) {
    long finalTicks;

    SysBeep(30);           // Queue beep for 30 ticks (~0.5 seconds)
    Delay(60, &finalTicks); // Wait 60 ticks (~1 second) for beep to complete
    ExitToShell();          // Clean exit
}
