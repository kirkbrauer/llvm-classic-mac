//===-- mem_string_test.c - Test memory/string functions ------------------===//
//
// Test program for macos_classic_mem.c functions.
// Single beep = all tests passed
// Multiple beeps = number of failed tests
//
//===----------------------------------------------------------------------===//

// Mac OS Toolbox
extern void SysBeep(short duration);
extern void Delay(unsigned long numTicks, unsigned long *finalTicks);
extern void ExitToShell(void) __attribute__((noreturn));

// Memory functions
extern void *memcpy(void *dest, const void *src, unsigned long n);
extern void *memmove(void *dest, const void *src, unsigned long n);
extern void *memset(void *s, int c, unsigned long n);
extern void bzero(void *s, unsigned long n);
extern int memcmp(const void *s1, const void *s2, unsigned long n);
extern int bcmp(const void *s1, const void *s2, unsigned long n);
extern void *memchr(const void *s, int c, unsigned long n);

// String functions
extern unsigned long strlen(const char *s);
extern char *strcpy(char *dest, const char *src);
extern char *strncpy(char *dest, const char *src, unsigned long n);
extern int strcmp(const char *s1, const char *s2);
extern int strncmp(const char *s1, const char *s2, unsigned long n);
extern char *strcat(char *dest, const char *src);
extern char *strncat(char *dest, const char *src, unsigned long n);
extern char *strchr(const char *s, int c);
extern char *strrchr(const char *s, int c);
extern char *strstr(const char *haystack, const char *needle);

// Test state
static int failures = 0;

static void fail(void) { failures++; }

//===----------------------------------------------------------------------===//
// Memory Function Tests
//===----------------------------------------------------------------------===//

static void test_memcpy(void) {
  char src[] = "Hello, World!";
  char dest[20];

  memcpy(dest, src, 14);
  if (dest[0] != 'H' || dest[7] != 'W' || dest[13] != '\0') {
    fail();
  }
}

static void test_memmove(void) {
  char buf[] = "ABCDEFGHIJ";

  // Overlapping copy forward
  memmove(buf + 2, buf, 5);
  if (buf[2] != 'A' || buf[6] != 'E') {
    fail();
  }
}

static void test_memset(void) {
  char buf[10];

  // Test non-zero fill
  memset(buf, 'X', 5);
  buf[5] = '\0';
  if (buf[0] != 'X' || buf[4] != 'X') {
    fail();
  }

  // Test zero fill (uses BlockZero)
  memset(buf, 0, 5);
  if (buf[0] != 0 || buf[4] != 0) {
    fail();
  }
}

static void test_bzero(void) {
  char buf[] = "XXXXXXXXXX";

  bzero(buf, 5);
  if (buf[0] != 0 || buf[4] != 0 || buf[5] != 'X') {
    fail();
  }
}

static void test_memcmp(void) {
  char a[] = "ABC";
  char b[] = "ABD";
  char c[] = "ABC";

  if (memcmp(a, c, 3) != 0) {
    fail();
  }
  if (memcmp(a, b, 3) >= 0) {
    fail();
  }
  if (memcmp(b, a, 3) <= 0) {
    fail();
  }
}

static void test_bcmp(void) {
  char a[] = "ABC";
  char b[] = "ABC";
  char c[] = "ABD";

  if (bcmp(a, b, 3) != 0) {
    fail();
  }
  if (bcmp(a, c, 3) == 0) {
    fail();
  }
}

static void test_memchr(void) {
  char buf[] = "Hello, World!";

  char *p = (char *)memchr(buf, 'W', 14);
  if (p != buf + 7) {
    fail();
  }

  p = (char *)memchr(buf, 'Z', 14);
  if (p != (char *)0) {
    fail();
  }
}

//===----------------------------------------------------------------------===//
// String Function Tests
//===----------------------------------------------------------------------===//

static void test_strlen(void) {
  if (strlen("") != 0) {
    fail();
  }
  if (strlen("Hello") != 5) {
    fail();
  }
  if (strlen("Hello, World!") != 13) {
    fail();
  }
}

static void test_strcpy(void) {
  char dest[20];

  strcpy(dest, "Hello");
  if (dest[0] != 'H' || dest[4] != 'o' || dest[5] != '\0') {
    fail();
  }
}

static void test_strncpy(void) {
  char dest[10];

  // Copy shorter string, should pad with nulls
  memset(dest, 'X', 10);
  strncpy(dest, "Hi", 5);
  if (dest[0] != 'H' || dest[1] != 'i' || dest[2] != '\0' || dest[4] != '\0') {
    fail();
  }

  // Copy longer string, should truncate
  strncpy(dest, "Hello, World!", 5);
  if (dest[0] != 'H' || dest[4] != 'o') {
    fail();
  }
}

static void test_strcmp(void) {
  if (strcmp("abc", "abc") != 0) {
    fail();
  }
  if (strcmp("abc", "abd") >= 0) {
    fail();
  }
  if (strcmp("abd", "abc") <= 0) {
    fail();
  }
  if (strcmp("", "") != 0) {
    fail();
  }
  if (strcmp("a", "") <= 0) {
    fail();
  }
}

static void test_strncmp(void) {
  if (strncmp("abcdef", "abcxxx", 3) != 0) {
    fail();
  }
  if (strncmp("abc", "abd", 3) >= 0) {
    fail();
  }
  if (strncmp("abc", "abc", 0) != 0) {
    fail();
  }
}

static void test_strcat(void) {
  char buf[20] = "Hello";

  strcat(buf, ", World!");
  if (buf[5] != ',' || buf[12] != '!' || buf[13] != '\0') {
    fail();
  }
}

static void test_strncat(void) {
  char buf[20] = "Hello";

  strncat(buf, ", World!", 3);
  if (buf[5] != ',' || buf[7] != 'W' || buf[8] != '\0') {
    fail();
  }
}

static void test_strchr(void) {
  const char *s = "Hello, World!";

  char *p = strchr(s, 'W');
  if (p != s + 7) {
    fail();
  }

  p = strchr(s, 'Z');
  if (p != (char *)0) {
    fail();
  }

  // Find null terminator
  p = strchr(s, '\0');
  if (p != s + 13) {
    fail();
  }
}

static void test_strrchr(void) {
  const char *s = "Hello, World!";

  char *p = strrchr(s, 'o');
  if (p != s + 8) {  // Last 'o' is in "World"
    fail();
  }

  p = strrchr(s, 'Z');
  if (p != (char *)0) {
    fail();
  }

  // Find null terminator
  p = strrchr(s, '\0');
  if (p != s + 13) {
    fail();
  }
}

static void test_strstr(void) {
  const char *s = "Hello, World!";

  char *p = strstr(s, "World");
  if (p != s + 7) {
    fail();
  }

  p = strstr(s, "Goodbye");
  if (p != (char *)0) {
    fail();
  }

  // Empty needle matches at start
  p = strstr(s, "");
  if (p != s) {
    fail();
  }

  // Full string match
  p = strstr(s, "Hello, World!");
  if (p != s) {
    fail();
  }
}

//===----------------------------------------------------------------------===//
// Main
//===----------------------------------------------------------------------===//

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;

  // Memory function tests
  test_memcpy();
  test_memmove();
  test_memset();
  test_bzero();
  test_memcmp();
  test_bcmp();
  test_memchr();

  // String function tests
  test_strlen();
  test_strcpy();
  test_strncpy();
  test_strcmp();
  test_strncmp();
  test_strcat();
  test_strncat();
  test_strchr();
  test_strrchr();
  test_strstr();

  // Beep to indicate results
  if (failures == 0) {
    // Success: single beep
    SysBeep(10);
    Delay(30, 0);
  } else {
    // Failure: beep once per failure (max 10)
    int beeps = failures > 10 ? 10 : failures;
    for (int i = 0; i < beeps; i++) {
      SysBeep(5);
      Delay(20, 0);
    }
  }

  ExitToShell();
}
