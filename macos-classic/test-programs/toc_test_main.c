//===-- toc_test_main.c - TOC Optimization Test Main Function ---*- C -*-===//
//
// Simple main function for TOC optimization testing
//
//===----------------------------------------------------------------------===//

// Forward declare SysBeep (cross fragment import)
extern void SysBeep(short duration);

// Main function (same fragment as __start)
int main(void) {
    // Call to SysBeep - import stub, linker should keep TOC restore
    SysBeep(30);

    return 0;
}
