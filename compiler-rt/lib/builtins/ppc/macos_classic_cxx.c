//===-- macos_classic_cxx.c - C++ Runtime Support --------------*- C -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Minimal C++ runtime support for PowerPC Classic Mac OS applications.
//
// This is a simplified implementation similar to Retro68/Newlib's approach.
// Function pointers are TVector addresses - the linker creates TVectors for
// all defined functions, enabling standard function pointer semantics.
//
//===----------------------------------------------------------------------===//

// No standard headers needed - freestanding implementation
#define NULL ((void *)0)

//===----------------------------------------------------------------------===//
// DSO Handle
//===----------------------------------------------------------------------===//

// DSO handle - points to itself for identification
void *__dso_handle = &__dso_handle;

//===----------------------------------------------------------------------===//
// Atexit Implementation (Simple Model)
//===----------------------------------------------------------------------===//

#define MAX_ATEXIT_HANDLERS 32

// Simple function pointer array - stores TVector addresses directly
// Initialize to force .data placement (not BSS)
static void (*atexit_handlers[MAX_ATEXIT_HANDLERS])(void) = {0};
static int atexit_count = 0;

// Standard C atexit - register function to call at exit
int atexit(void (*func)(void)) {
  if (func == NULL || atexit_count >= MAX_ATEXIT_HANDLERS)
    return -1;

  atexit_handlers[atexit_count++] = func;
  return 0;
}

//===----------------------------------------------------------------------===//
// C++ ABI Functions
//===----------------------------------------------------------------------===//

// __cxa_atexit: Register destructor (simplified - ignores arg/dso)
int __cxa_atexit(void (*func)(void *), void *arg, void *dso) {
  (void)arg;
  (void)dso;
  return atexit((void (*)(void))func);
}

// __cxa_finalize: Run registered handlers in reverse order
void __cxa_finalize(void *dso) {
  (void)dso;

  // Run handlers in reverse order (LIFO)
  while (atexit_count > 0) {
    atexit_count--;
    void (*handler)(void) = atexit_handlers[atexit_count];
    atexit_handlers[atexit_count] = NULL;
    if (handler) {
      handler();  // Call through function pointer (TVector)
    }
  }
}

//===----------------------------------------------------------------------===//
// Static Initialization Guards (Simplified - no threading)
//===----------------------------------------------------------------------===//

__attribute__((weak)) int __cxa_guard_acquire(int *guard) {
  if (*guard)
    return 0;
  *guard = 1;
  return 1;
}

__attribute__((weak)) void __cxa_guard_release(int *guard) {
  *guard = 1;
}

__attribute__((weak)) void __cxa_guard_abort(int *guard) {
  *guard = 0;
}

//===----------------------------------------------------------------------===//
// Pure Virtual Handlers
//===----------------------------------------------------------------------===//

__attribute__((weak)) void __cxa_pure_virtual(void) {
  while (1);  // Programming error - hang
}

__attribute__((weak)) void __cxa_deleted_virtual(void) {
  while (1);  // Programming error - hang
}

//===----------------------------------------------------------------------===//
// Exception Frame Stubs
//===----------------------------------------------------------------------===//

__attribute__((weak)) void __register_frame_info(const void *frame, void *obj) {
  (void)frame;
  (void)obj;
}

__attribute__((weak)) void *__deregister_frame_info(const void *frame) {
  (void)frame;
  return NULL;
}

//===----------------------------------------------------------------------===//
// Exit Function
//===----------------------------------------------------------------------===//

extern void ExitToShell(void) __attribute__((noreturn));

void exit(int status) {
  (void)status;
  __cxa_finalize(__dso_handle);
  ExitToShell();
  __builtin_unreachable();
}
