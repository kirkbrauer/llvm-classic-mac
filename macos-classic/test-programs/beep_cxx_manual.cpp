// beep_cxx_manual.cpp - C++ Runtime Test with Manual Constructor/Destructor
//
// Tests __cxa_atexit and __dso_handle with manually-invoked constructors/destructors
// This works around the limitation that PEF doesn't support .ctors/.init_array sections
//
// Expected beep sequence:
// 1. Short beep (10 ticks) - Manual constructor call
// 2. Long beep (30 ticks) - Main function execution
// 3. Medium beep (20 ticks) - First destructor beep (via __cxa_atexit)
// 4. Medium beep (20 ticks) - Second destructor beep
//
// Total: 4 beeps in sequence = SUCCESS

extern "C" {
    void SysBeep(short duration);
    void Delay(long numTicks, long *finalTicks);
}

// Placement new operator (needed for manual construction)
inline void* operator new(unsigned long, void* ptr) { return ptr; }

// C++ class with constructor and destructor
class BeepOnExit {
public:
    BeepOnExit() {
        SysBeep(10);  // Short beep in constructor
    }

    ~BeepOnExit() {
        SysBeep(20);  // Medium beep
        Delay(30, 0);
        SysBeep(20);  // Medium beep
    }
};

// Static destructor wrapper that will be registered with __cxa_atexit
// This is what the compiler would normally generate automatically
static void destruct_beeper(void *obj) {
    // Cast back to BeepOnExit* and call destructor
    // NOTE: Current __cxa_atexit implementation doesn't pass obj correctly,
    // so we use the global instance instead
    static_cast<BeepOnExit*>(obj)->~BeepOnExit();
}

// Storage for our "global" object (not really global to avoid .ctors)
static char beeper_storage[sizeof(BeepOnExit)] __attribute__((aligned(8)));
static BeepOnExit *beeper_ptr = nullptr;

int main(int argc, char **argv) {
    // Manually construct the object (simulates what .ctors would do)
    beeper_ptr = new (beeper_storage) BeepOnExit();  // Placement new

    // Register destructor with __cxa_atexit
    // This is what the compiler would do automatically for global objects
    __cxa_atexit(destruct_beeper, beeper_ptr, &__dso_handle);

    // Main function logic
    SysBeep(30);   // Long beep
    Delay(30, 0);

    // Return triggers exit() → __cxa_finalize(__dso_handle) → destruct_beeper()
    return 0;
}

// Expected execution timeline:
// [Program Start]
//     ↓
// main() entry
//     ↓
// placement new → BeepOnExit::BeepOnExit() → **BEEP 1** (short, 10 ticks)
//     ↓
// __cxa_atexit(destruct_beeper, beeper_ptr, &__dso_handle)
//     ↓
// SysBeep(30) → **BEEP 2** (long, 30 ticks)
//     ↓
// return 0 → exit(0) → __cxa_finalize(__dso_handle)
//     ↓
// destruct_beeper(beeper_ptr) → BeepOnExit::~BeepOnExit()
//     ↓
// SysBeep(20) → **BEEP 3** (medium, 20 ticks)
//     ↓
// Delay(30, 0)
//     ↓
// SysBeep(20) → **BEEP 4** (medium, 20 ticks)
//     ↓
// ExitToShell()
