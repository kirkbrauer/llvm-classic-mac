//===-- macos_classic_mem.c - Memory/String Functions for Classic Mac OS --===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// C standard library memory and string functions for PowerPC Classic Mac OS.
//
// This file provides implementations of memset, memcpy, memmove, and other
// essential C library functions needed by Rust's core library and C programs.
//
// Where possible, we use Mac OS Toolbox functions from InterfaceLib:
//   - BlockMove: Copy memory, handles overlapping regions
//   - BlockMoveData: Copy memory without cache flush (faster)
//   - BlockZero: Zero-fill memory
//
// For functions without Toolbox equivalents (comparisons, string ops),
// we provide simple portable C implementations.
//
//===----------------------------------------------------------------------===//

// Freestanding type definitions (no system headers)
typedef unsigned long size_t;

#ifndef NULL
#define NULL ((void *)0)
#endif

//===----------------------------------------------------------------------===//
// InterfaceLib Function Declarations
//===----------------------------------------------------------------------===//

// Note: BlockMove/BlockMoveData parameter order is (src, dest, count)
// which is REVERSED from memcpy(dest, src, count)!
extern void BlockMove(const void *srcPtr, void *destPtr, long byteCount);
extern void BlockMoveData(const void *srcPtr, void *destPtr, long byteCount);
extern void BlockZero(void *destPtr, long byteCount);

//===----------------------------------------------------------------------===//
// Memory Copy Functions
//===----------------------------------------------------------------------===//

// memcpy: Copy n bytes from src to dest (non-overlapping)
// Uses BlockMoveData for efficiency (no cache flush)
void *memcpy(void *dest, const void *src, size_t n) {
  if (n > 0) {
    BlockMoveData(src, dest, (long)n);
  }
  return dest;
}

// memmove: Copy n bytes from src to dest (handles overlap)
// BlockMove properly handles overlapping regions
void *memmove(void *dest, const void *src, size_t n) {
  if (n > 0) {
    BlockMove(src, dest, (long)n);
  }
  return dest;
}

//===----------------------------------------------------------------------===//
// Memory Set Functions
//===----------------------------------------------------------------------===//

// memset: Fill n bytes of s with byte value c
// Uses BlockZero for c=0, otherwise byte-by-byte fill
void *memset(void *s, int c, size_t n) {
  if (n == 0) {
    return s;
  }

  if (c == 0) {
    // Use optimized BlockZero for zero-fill
    BlockZero(s, (long)n);
  } else {
    // Byte-by-byte fill for non-zero values
    unsigned char *p = (unsigned char *)s;
    unsigned char byte = (unsigned char)c;
    while (n--) {
      *p++ = byte;
    }
  }
  return s;
}

// bzero: Zero n bytes starting at s (BSD function)
void bzero(void *s, size_t n) {
  if (n > 0) {
    BlockZero(s, (long)n);
  }
}

//===----------------------------------------------------------------------===//
// Memory Comparison Functions
//===----------------------------------------------------------------------===//

// memcmp: Compare n bytes of s1 and s2
// Returns <0 if s1<s2, 0 if equal, >0 if s1>s2
int memcmp(const void *s1, const void *s2, size_t n) {
  const unsigned char *p1 = (const unsigned char *)s1;
  const unsigned char *p2 = (const unsigned char *)s2;

  while (n--) {
    if (*p1 != *p2) {
      return (int)*p1 - (int)*p2;
    }
    p1++;
    p2++;
  }
  return 0;
}

// bcmp: Compare n bytes (BSD function, returns 0 if equal, non-zero otherwise)
int bcmp(const void *s1, const void *s2, size_t n) {
  return memcmp(s1, s2, n);
}

//===----------------------------------------------------------------------===//
// Memory Search Functions
//===----------------------------------------------------------------------===//

// memchr: Find first occurrence of byte c in first n bytes of s
void *memchr(const void *s, int c, size_t n) {
  const unsigned char *p = (const unsigned char *)s;
  unsigned char byte = (unsigned char)c;

  while (n--) {
    if (*p == byte) {
      return (void *)p;
    }
    p++;
  }
  return NULL;
}

//===----------------------------------------------------------------------===//
// String Functions
//===----------------------------------------------------------------------===//

// strlen: Return length of null-terminated string s
size_t strlen(const char *s) {
  const char *start = s;
  while (*s) {
    s++;
  }
  return (size_t)(s - start);
}

// strcpy: Copy string src to dest (including null terminator)
char *strcpy(char *dest, const char *src) {
  char *d = dest;
  while ((*d++ = *src++) != '\0') {
    // Copy continues until null terminator is copied
  }
  return dest;
}

// strncpy: Copy at most n characters from src to dest
// Pads with null bytes if src is shorter than n
char *strncpy(char *dest, const char *src, size_t n) {
  char *d = dest;

  // Copy up to n characters from src
  while (n > 0 && *src != '\0') {
    *d++ = *src++;
    n--;
  }

  // Pad remainder with null bytes
  while (n > 0) {
    *d++ = '\0';
    n--;
  }

  return dest;
}

// strcmp: Compare two null-terminated strings
// Returns <0 if s1<s2, 0 if equal, >0 if s1>s2
int strcmp(const char *s1, const char *s2) {
  while (*s1 && (*s1 == *s2)) {
    s1++;
    s2++;
  }
  return (int)(unsigned char)*s1 - (int)(unsigned char)*s2;
}

// strncmp: Compare at most n characters of two strings
int strncmp(const char *s1, const char *s2, size_t n) {
  if (n == 0) {
    return 0;
  }

  while (n > 1 && *s1 && (*s1 == *s2)) {
    s1++;
    s2++;
    n--;
  }
  return (int)(unsigned char)*s1 - (int)(unsigned char)*s2;
}

// strcat: Append src string to end of dest string
char *strcat(char *dest, const char *src) {
  char *d = dest;

  // Find end of dest string
  while (*d) {
    d++;
  }

  // Copy src to end of dest
  while ((*d++ = *src++) != '\0') {
    // Copy continues until null terminator is copied
  }

  return dest;
}

// strncat: Append at most n characters from src to dest
char *strncat(char *dest, const char *src, size_t n) {
  char *d = dest;

  // Find end of dest string
  while (*d) {
    d++;
  }

  // Copy up to n characters from src
  while (n > 0 && *src != '\0') {
    *d++ = *src++;
    n--;
  }

  // Always null-terminate
  *d = '\0';

  return dest;
}

// strchr: Find first occurrence of character c in string s
char *strchr(const char *s, int c) {
  char ch = (char)c;

  while (*s) {
    if (*s == ch) {
      return (char *)s;
    }
    s++;
  }

  // Check for null terminator match
  if (ch == '\0') {
    return (char *)s;
  }

  return (char *)NULL;
}

// strrchr: Find last occurrence of character c in string s
char *strrchr(const char *s, int c) {
  char ch = (char)c;
  const char *last = (const char *)NULL;

  while (*s) {
    if (*s == ch) {
      last = s;
    }
    s++;
  }

  // Check for null terminator match
  if (ch == '\0') {
    return (char *)s;
  }

  return (char *)last;
}

// strstr: Find first occurrence of needle in haystack
char *strstr(const char *haystack, const char *needle) {
  // Empty needle matches at start
  if (*needle == '\0') {
    return (char *)haystack;
  }

  while (*haystack) {
    const char *h = haystack;
    const char *n = needle;

    // Try to match needle starting at current position
    while (*h && *n && (*h == *n)) {
      h++;
      n++;
    }

    // If we reached end of needle, we found a match
    if (*n == '\0') {
      return (char *)haystack;
    }

    haystack++;
  }

  return (char *)NULL;
}
