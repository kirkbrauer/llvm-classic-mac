// beep_cxx_test.cpp - C++ Runtime Test with Beep Feedback
//
// Tests self-referential __dso_handle and C++ runtime features:
// - Global object construction (before main)
// - __cxa_atexit registration
// - __cxa_finalize destructor execution (after main)
// - LIFO destructor order
//
// Expected beep sequence:
// 1. Short beep (10 ticks) - Global constructor runs before main()
// 2. Long beep (30 ticks) - Main function execution
// 3. Medium beep (20 ticks) - First destructor beep
// 4. Medium beep (20 ticks) - Second destructor beep
//
// Total: 4 beeps in sequence = SUCCESS
//
// If only 2 beeps: Destructors not running (DSO handle issue)
// If crash after beep 2: __cxa_finalize or destructor crash

// Mac OS Toolbox API declarations
// Use extern "C" to avoid C++ name mangling
extern "C" {
    // Play a system beep
    // duration: length in ticks (60 ticks = 1 second)
    void SysBeep(short duration);

    // Delay for a specified number of ticks
    // numTicks: number of ticks to wait (60 ticks = 1 second)
    // finalTicks: pointer to receive final tick count (can be NULL)
    void Delay(long numTicks, long *finalTicks);
}

// Global C++ object that beeps on construction and destruction
// This tests:
// - Static initialization (constructor called before main)
// - __cxa_atexit registration (destructor registered for cleanup)
// - __dso_handle usage (destructor associated with this DSO)
// - __cxa_finalize execution (destructor called during exit)
class BeepOnExit {
public:
    // Constructor - called during static initialization before main()
    // Plays a short beep to indicate global object construction
    BeepOnExit() {
        SysBeep(10);  // Short beep (10 ticks ≈ 0.17 seconds)
    }

    // Destructor - registered via __cxa_atexit, called by __cxa_finalize
    // Plays two medium beeps to indicate cleanup is running
    //
    // NOTE: Current __cxa_atexit implementation doesn't pass 'this' pointer
    // correctly, so we can't access member variables here. This is a known
    // limitation that should be fixed in the future.
    ~BeepOnExit() {
        SysBeep(20);     // Medium beep (20 ticks ≈ 0.33 seconds)
        Delay(30, 0);    // Wait 0.5 seconds between beeps
        SysBeep(20);     // Medium beep again
    }
};

// Static global object - tests C++ static initialization and destruction
// Constructor runs before main(), destructor runs after main() returns
static BeepOnExit globalBeeper;

// Main entry point
// The flow is: __start() -> main() -> exit() -> __cxa_finalize() -> ExitToShell()
int main(int argc, char **argv) {
    // Play a long beep to indicate we're in main()
    // By this point, the global constructor should have already run
    SysBeep(30);      // Long beep (30 ticks ≈ 0.5 seconds)
    Delay(30, 0);     // Wait 0.5 seconds

    // Return 0 triggers exit() which calls __cxa_finalize(__dso_handle)
    // This should run the global destructor (playing 2 more beeps)
    return 0;
}

// Expected execution timeline:
//
// [Program Start]
//     ↓
// [Static Initialization]
//     ↓
// BeepOnExit::BeepOnExit() → **BEEP 1** (short, 10 ticks)
//     ↓
// main() entry
//     ↓
// SysBeep(30) → **BEEP 2** (long, 30 ticks)
//     ↓
// Delay(30, 0)
//     ↓
// return 0
//     ↓
// exit(0)
//     ↓
// __cxa_finalize(__dso_handle)
//     ↓
// BeepOnExit::~BeepOnExit()
//     ↓
// SysBeep(20) → **BEEP 3** (medium, 20 ticks)
//     ↓
// Delay(30, 0)
//     ↓
// SysBeep(20) → **BEEP 4** (medium, 20 ticks)
//     ↓
// ExitToShell()
//     ↓
// [Program End]
