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

class BeepOnExit {
public:
    int Count;  // Just a sample member variable

    BeepOnExit() : Count(0) { }

    void setCount(int c) {
        Count = c;
    }

    void beepCount() {
        for (int i = 0; i < Count; ++i) {
            SysBeep(10);  // Short beep (10 ticks ≈ 0.16 seconds)
            Delay(20, 0); // Wait 0.33 seconds between beeps
        }
    }
};

// Main entry point
// The flow is: __start() -> main() -> exit() -> __cxa_finalize() -> ExitToShell()
int main(int argc, char **argv) {
    BeepOnExit beeper;
    beeper.setCount(2); // Set to beep twice on destruction
    beeper.beepCount(); // Beep twice to indicate main execution
    return 0;
}
