// Test Level 2: Toolbox API call - SysBeep
// SysBeep is a classic Mac OS Toolbox function that makes a system beep sound

// Forward declaration - avoid pulling in full MacTypes.h for now
void SysBeep(short duration);

int main(void) {
    SysBeep(30);  // Beep for 30 ticks (approx 0.5 seconds)
    return 0;
}
