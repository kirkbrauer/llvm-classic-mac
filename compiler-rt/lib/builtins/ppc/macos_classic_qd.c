//===-- macos_classic_qd.c - QuickDraw Globals Storage ---------*- C -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// QuickDraw globals storage for Classic Mac OS applications.
//
// The QuickDraw graphics system requires applications to provide storage
// for the QDGlobals structure. The Mac OS Toolbox initializes this structure
// via the InitGraf(&qd.thePort) call at application startup.
//
// This is NOT provided by InterfaceLib - it's application-provided storage.
// Size: Approximately 206 bytes (76 private + 4 randSeed + 14 screenBits +
//                                 68 arrow cursor + 40 patterns + 4 thePort)
//
// Note: Carbon applications do not use this - they use accessor functions
// to access Toolbox-maintained state instead.
//
//===----------------------------------------------------------------------===//

// Check if QuickDraw header is available
#if __has_include(<Quickdraw.h>)
#include <Quickdraw.h>
#else
// Minimal definition if Mac OS headers not available
// Based on Universal Interfaces 3.4.2 specification
typedef struct QDGlobals {
  char privates[76];     // Private data used by QuickDraw
  long randSeed;         // Random seed for pattern operations
  char screenBits[14];   // Screen bitmap (BitMap structure)
  char arrow[68];        // Standard arrow cursor (Cursor structure)
  char dkGray[8];        // Dark gray pattern
  char ltGray[8];        // Light gray pattern
  char gray[8];          // Medium gray pattern
  char black[8];          // Black pattern
  char white[8];         // White pattern
  void *thePort;         // Current graphics port pointer
} QDGlobals;
#endif

// Global QuickDraw state storage
// Must be passed to InitGraf() at application startup:
//   InitGraf(&qd.thePort);
QDGlobals qd;
