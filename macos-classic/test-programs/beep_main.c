//===-- beep_main.c - Main Function for Split Test --------------*- C -*-===//
//
// This file contains the main() function
// Will be linked with beep_start.c to test multi-file linking
//
//===----------------------------------------------------------------------===//

#include <Sound.h>
#include <OSUtils.h>

int main(void) {
    // Beep for 30 ticks (half second)
    SysBeep(30);

    // Delay for 60 ticks (1 second)
    Delay(60, NULL);

    // Return to __start
    return 0;
}
