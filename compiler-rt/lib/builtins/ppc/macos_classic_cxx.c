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
// Implements essential C++ ABI functions as defined by the Itanium C++ ABI,
// which is the standard used by GCC and Clang for non-MSVC platforms:
// https://itanium-cxx-abi.github.io/cxx-abi/abi.html
//
// Provides:
// - __dso_handle: DSO identifier for tracking destructors
// - atexit: Standard C atexit function
// - __cxa_atexit: C++ destructor registration
// - __cxa_finalize: C++ destructor execution
// - __cxa_guard_*: Thread-safe static initialization (simplified)
// - __cxa_pure_virtual: Pure virtual function call handler
//
// Optional features not implemented:
// - Exception handling (__cxa_throw, __cxa_catch, etc.)
// - RTTI (__dynamic_cast, type_info, etc.)
// - Thread-local storage
//
// Applications should compile with -fno-exceptions and -fno-rtti for
// minimal binary size and best compatibility with Classic Mac OS.
//
//===----------------------------------------------------------------------===//

#include <stddef.h>

//===----------------------------------------------------------------------===//
// DSO Handle
//===----------------------------------------------------------------------===//

// DSO (Dynamic Shared Object) handle for this executable.
// This is a unique identifier used by the C++ runtime to track which
// destructors belong to which linkage unit. For a standalone executable,
// this is simply a self-referential pointer.
//
// Required by __cxa_atexit to associate destructors with this executable.
void *__dso_handle = &__dso_handle;

//===----------------------------------------------------------------------===//
// Atexit Implementation
//===----------------------------------------------------------------------===//

// Maximum number of functions that can be registered with atexit.
// Can be increased if needed, but 128 should be sufficient for most apps.
#define MAX_ATEXIT_HANDLERS 128

typedef void (*atexit_func_t)(void);

static atexit_func_t atexit_handlers[MAX_ATEXIT_HANDLERS];
static int atexit_count = 0;

// Standard C atexit function
// Registers a function to be called at program termination.
//
// Returns:
//   0 on success
//   -1 if the handler table is full
int atexit(atexit_func_t func) {
  if (func == NULL)
    return -1;
  if (atexit_count >= MAX_ATEXIT_HANDLERS)
    return -1;

  atexit_handlers[atexit_count++] = func;
  return 0;
}

//===----------------------------------------------------------------------===//
// C++ ABI Functions
//===----------------------------------------------------------------------===//

// __cxa_atexit: Register a destructor for a global or static object
//
// This is called by compiler-generated code to register destructors for
// C++ objects with static storage duration. The function will be called
// with the given argument when the program exits or when __cxa_finalize
// is called for the associated DSO.
//
// Parameters:
//   func: Destructor function pointer (takes void* argument)
//   arg: Argument to pass to destructor (usually 'this' pointer)
//   dso: DSO handle (__dso_handle for this executable)
//
// Returns:
//   0 on success
//   -1 on failure
//
// Note: This simplified implementation ignores the arg and dso parameters
// and casts the function pointer to void(void). A full implementation would
// store all three values and check dso in __cxa_finalize.
int __cxa_atexit(void (*func)(void *), void *arg, void *dso) {
  // For a single executable (not supporting dynamic libraries),
  // we can simplify by ignoring arg and dso
  (void)arg;
  (void)dso;

  return atexit((atexit_func_t)func);
}

// __cxa_finalize: Run destructors for a DSO
//
// Called during program cleanup to run all registered destructors.
// Destructors are run in reverse order of registration (LIFO).
//
// Parameters:
//   dso: DSO handle to finalize, or NULL to finalize all
//
// Note: This simplified implementation ignores the dso parameter and
// runs all registered destructors.
void __cxa_finalize(void *dso) {
  (void)dso;

  // Run all registered handlers in reverse order
  for (int i = atexit_count - 1; i >= 0; i--) {
    if (atexit_handlers[i])
      atexit_handlers[i]();
  }

  atexit_count = 0;
}

//===----------------------------------------------------------------------===//
// Static Initialization Guards
//===----------------------------------------------------------------------===//

// __cxa_guard_acquire: Acquire guard for static initialization
//
// Used for thread-safe initialization of function-local statics.
// Classic Mac OS doesn't have preemptive multithreading, so this is
// simplified to just check if initialization has occurred.
//
// Parameters:
//   guard: Pointer to guard variable (4-byte integer)
//
// Returns:
//   1 if the caller should initialize the static
//   0 if already initialized
__attribute__((weak)) int __cxa_guard_acquire(int *guard) {
  // Check if already initialized
  if (*guard)
    return 0; // Already initialized, don't re-initialize

  // Mark as in-progress and tell caller to initialize
  *guard = 1;
  return 1;
}

// __cxa_guard_release: Release guard after successful initialization
//
// Called after static initialization completes successfully.
//
// Parameters:
//   guard: Pointer to guard variable
__attribute__((weak)) void __cxa_guard_release(int *guard) {
  // Mark as fully initialized
  *guard = 1;
}

// __cxa_guard_abort: Abort initialization (called on exception)
//
// Called if static initialization fails (throws exception).
// Resets the guard so initialization can be retried.
//
// Parameters:
//   guard: Pointer to guard variable
__attribute__((weak)) void __cxa_guard_abort(int *guard) {
  // Reset to uninitialized state
  *guard = 0;
}

//===----------------------------------------------------------------------===//
// Pure Virtual Function Handler
//===----------------------------------------------------------------------===//

// __cxa_pure_virtual: Called when a pure virtual function is invoked
//
// This should never happen in correct code, but if it does, we need
// to handle it gracefully (or at least predictably).
//
// In a full implementation, this would call Mac OS error reporting
// (e.g., DebugStr or ExitToShell). For now, infinite loop.
__attribute__((weak)) void __cxa_pure_virtual(void) {
  // Pure virtual function called - this is a programming error
  // TODO: Call Mac OS DebugStr or show error dialog
  while (1)
    ; // Infinite loop
}

// __cxa_deleted_virtual: Called when a deleted virtual function is invoked
//
// Similar to __cxa_pure_virtual but for explicitly deleted functions (C++11).
__attribute__((weak)) void __cxa_deleted_virtual(void) {
  // Deleted virtual function called - this is a programming error
  while (1)
    ; // Infinite loop
}

//===----------------------------------------------------------------------===//
// Exception Handling Frame Registration (Stubs)
//===----------------------------------------------------------------------===//

// These functions are used for C++ exception handling and are typically
// provided by libgcc or compiler-rt's unwind library. We provide weak
// stubs so that applications can link without them.
//
// Applications should use -fno-exceptions to avoid generating exception
// handling code, as Classic Mac OS doesn't support exceptions well.

__attribute__((weak)) void __register_frame_info(const void *frame,
                                                  void *obj) {
  // Exception handling frame registration - stub
  (void)frame;
  (void)obj;
}

__attribute__((weak)) void *__deregister_frame_info(const void *frame) {
  // Exception handling frame cleanup - stub
  (void)frame;
  return NULL;
}
