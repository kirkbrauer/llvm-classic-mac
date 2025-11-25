// Simplified beep test: SysBeep + ExitToShell only
// This test validates:
// - Two function imports (no Delay)
// - Single parameter passing (short value, no pointers)
// - No local variables on stack
//
// Purpose: Isolate whether crash is due to:
// - Multi-import handling (2 vs 1)
// - Parameter passing (value param vs no param)
// - OR pointer/local variable issues (eliminated here)

void SysBeep(short duration);
void ExitToShell(void);

void __start(void) {
    SysBeep(30);       // Single short parameter - beep for 30 ticks (~0.5 sec)
    ExitToShell();     // No parameters - clean exit
}
