//===-- function_pointer_internal_test.c - Test internal function ptr --*- C -*-===//
//
// Test taking address of INTERNAL function (not imported)
// This should work with our current linker implementation
//
//===----------------------------------------------------------------------===//

extern void ExitToShell(void) __attribute__((noreturn));

// Internal helper function
void helper_function(void) {
    ExitToShell();
}

// Global function pointer to INTERNAL function
void (*func_ptr)(void) = 0;

void __start(void) {
    // Take address of internal function
    func_ptr = &helper_function;

    // Call through pointer
    func_ptr();

    __builtin_unreachable();
}
