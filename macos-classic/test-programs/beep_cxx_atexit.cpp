// beep_cxx_atexit.cpp - C++ Runtime Test using atexit()
//
// Tests C++ destructors registered via standard atexit() function
// This is simpler than __cxa_atexit and should work with current runtime
//
// Expected beep sequence with LONG delays for clarity:
// 1. Short beep (10 ticks) - Manual constructor call
// 2. [PAUSE 2 seconds]
// 3. Long beep (30 ticks) - Main function execution
// 4. [PAUSE 2 seconds]
// 5. Medium beep (20 ticks) - First destructor beep (via atexit)
// 6. [PAUSE 2 seconds]
// 7. Medium beep (20 ticks) - Second destructor beep
//
// Total: 4 beeps with clear pauses = SUCCESS

extern "C" {
    void SysBeep(short duration);
    void Delay(long numTicks, long *finalTicks);
    int atexit(void (*func)(void));
}

// Placement new operator
inline void* operator new(unsigned long, void* ptr) { return ptr; }

// C++ class with constructor and destructor
class BeepOnExit {
public:
    BeepOnExit() {
        SysBeep(10);  // Short beep in constructor
        Delay(120, 0);  // LONG PAUSE: 2 seconds
    }

    ~BeepOnExit() {
        SysBeep(20);  // Medium beep
        Delay(120, 0);  // LONG PAUSE: 2 seconds
        SysBeep(20);  // Medium beep
    }

    // Static method to call destructor (for atexit)
    static void destroy() {
        if (instance) {
            instance->~BeepOnExit();
            instance = nullptr;
        }
    }

    static BeepOnExit* instance;
};

// Static instance pointer
BeepOnExit* BeepOnExit::instance = nullptr;

// Storage for our object
static char beeper_storage[sizeof(BeepOnExit)] __attribute__((aligned(8)));

int main(int argc, char **argv) {
    // Manually construct the object using placement new
    BeepOnExit::instance = new (beeper_storage) BeepOnExit();
    // Constructor plays SHORT beep + 2 second pause

    // Register destructor with atexit
    // atexit() expects void (*)(void), which our static method provides
    atexit(BeepOnExit::destroy);

    // Main function logic
    SysBeep(30);   // LONG beep
    Delay(120, 0);  // LONG PAUSE: 2 seconds

    // Return triggers exit() → runs atexit handlers → BeepOnExit::destroy()
    // destroy() calls destructor which plays: MEDIUM beep + 2s pause + MEDIUM beep
    return 0;
}
