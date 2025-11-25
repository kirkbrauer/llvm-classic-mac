//===-- beep_main_fixed.c - Main Function for Split Test (Fixed) -*-C-*-===//
//
// Main function that returns to __start for testing LR save/restore fix
//
//===----------------------------------------------------------------------===//

// Forward declarations
void SysBeep(short duration);
void Delay(long numTicks, long *finalTicks);

int main(void) {
    // Beep for 30 ticks (half second)
    SysBeep(30);

    // Delay for 60 ticks (1 second)
    Delay(60, 0);

    // Return to __start
    return 0;
}
