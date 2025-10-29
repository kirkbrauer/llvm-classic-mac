# RUN: llvm-mc -triple=powerpc-apple-macos -filetype=obj < %s | llvm-readobj --file-headers - | FileCheck %s

# Test that we can generate PEF object files for Classic Mac OS targets

# CHECK: Format: pef_object

.text
.globl _start
_start:
    li r3, 42
    blr

.data
.globl _hello
_hello:
    .long 0x48656c6c  # "Hell"
    .long 0x6f210000  # "o!\0\0"
