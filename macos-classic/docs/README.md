# Mac OS Classic PEF Documentation

This directory contains documentation for the LLVM PEF (Preferred Executable Format) toolchain, enabling Classic Mac OS (System 7 through Mac OS 9) PowerPC development using modern LLVM infrastructure.

## Core Documentation

- **[PEF_FORMAT_SPECIFICATION.md](PEF_FORMAT_SPECIFICATION.md)** - Complete PEF format reference
  - Container and section header structures
  - Transition Vector (TVect) format
  - Relocation bytecode system
  - Loader section layout
  - CFM (Code Fragment Manager) loading process

- **[LLVM_PEF_ARCHITECTURE.md](LLVM_PEF_ARCHITECTURE.md)** - LLVM implementation deep dive
  - Build pipeline (Clang -> LLVM IR -> PEF)
  - Component architecture (Driver, Writer, RelocWriter)
  - Import stub implementation
  - Data section layout

- **[PEF_RELOCATIONS.md](PEF_RELOCATIONS.md)** - Relocation bytecode format
  - Bytecode interpreter model
  - Relocation opcodes
  - Section-relative addressing

## Reference

- **[RETRO68_APPROACH.md](RETRO68_APPROACH.md)** - How Retro68 works (for comparison)
  - XCOFF -> PEF conversion pipeline
  - MakePEF tool analysis
  - Import library encoding

- **[PEF_IMPLEMENTATION_COMPARISON.md](PEF_IMPLEMENTATION_COMPARISON.md)** - LLVM vs Retro68 comparison
  - Binary size analysis
  - Import stub differences
  - Trade-offs and recommendations

## Testing

- **[TESTING_GUIDE.md](TESTING_GUIDE.md)** - Testing procedures and workflow
  - Build testing with Makefile
  - Static analysis with llvm-readobj
  - Runtime testing in SheepShaver
  - Troubleshooting guide

## Quick Reference

### Build a test program

```bash
cd llvm-project
make test NAME=minimal_test
make prepare NAME=minimal_test
```

### Inspect a PEF binary

```bash
./build/bin/llvm-readobj --pef-header binary.pef
./build/bin/llvm-objdump --disassemble binary.pef
```

## Apple Official Documentation

- **[rt-architectures/](rt-architectures/)** - Complete "Mac OS Runtime Architectures" book (converted from HTML)
  - [CFM-Based Runtime Architecture](rt-architectures/01-cfm-based-runtime-architecture-the-cfm-based-runti.md)
  - [Indirect Addressing](rt-architectures/02-indirect-addressing-in-the-cfm-based-architecture-.md)
  - [PowerPC Runtime Conventions](rt-architectures/04-powerpc-runtime-conventions-this-chapter-covers-sp.md)
  - [**PEF Structure**](rt-architectures/08-pef-structure-this-chapter-describes-the-structure.md) - Most critical for LLVM implementation
  - [Full Index](rt-architectures/index.md)
