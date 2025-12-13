//===-- mem_simple_test.c - Simple memory function test -------------------===//
//
// Minimal test to diagnose memory function issues.
// Beeps indicate progress through tests.
//
//===----------------------------------------------------------------------===//

// Mac OS Toolbox
extern void SysBeep(short duration);
extern void Delay(unsigned long numTicks, unsigned long *finalTicks);
extern void ExitToShell(void) __attribute__((noreturn));

// Memory functions
extern void *memset(void *s, int c, unsigned long n);
extern void *memcpy(void *dest, const void *src, unsigned long n);
extern unsigned long strlen(const char *s);

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;

  // Beep 1: We made it to main
  SysBeep(10);
  Delay(30, 0);

  // Test 1: Simple memset with zero
  char buf1[16];
  memset(buf1, 0, 16);

  // Beep 2: memset(0) worked
  SysBeep(10);
  Delay(30, 0);

  // Test 2: memset with non-zero
  char buf2[16];
  memset(buf2, 'A', 8);
  buf2[8] = '\0';

  // Beep 3: memset(non-zero) worked
  SysBeep(10);
  Delay(30, 0);

  // Test 3: memcpy
  char src[] = "Hello";
  char dst[16];
  memcpy(dst, src, 6);

  // Beep 4: memcpy worked
  SysBeep(10);
  Delay(30, 0);

  // Test 4: strlen
  unsigned long len = strlen("Test");
  (void)len;

  // Beep 5: strlen worked - all tests passed!
  SysBeep(10);
  Delay(30, 0);

  ExitToShell();
}
