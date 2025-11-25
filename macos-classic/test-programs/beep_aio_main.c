//===-- beep_main.c - Main Function for Split Test --------------*- C -*-===//
//
// This file contains the main() function
// Will be linked with beep_start.c to test multi-file linking
//
//===----------------------------------------------------------------------===//

void ExitToShell();
void SysBeep(short duration);
void Delay(long numTicks, long *finalTicks);

int main(int argc, char **argv) {
    // Beep for 30 ticks (half second)
    SysBeep(30);

    // Delay for 60 ticks (1 second)
    Delay(60, 0);

    // Return to __start
    return 0;
}

void __start(void) {
  // Classic Mac applications don't have command-line arguments
  // Provide minimal argc/argv for compatibility with standard main()
  char *argv[2] = {"app", (char *)0};

  // Call the application's main function
  int result = main(1, argv);

  // Return to Code Fragment Manager
  // CFM will handle final cleanup and return to Mac OS
  // Note: Classic Mac OS doesn't use the return value
  (void)result;
  ExitToShell();
}

