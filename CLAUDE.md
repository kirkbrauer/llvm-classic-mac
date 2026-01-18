# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is **llvm-project** with native support for Classic Mac OS development:
- **PEF (Preferred Executable Format)** for PowerPC (System 7 through Mac OS 9)
- **Classic 68K** for Motorola 68000 series (System 6 through Mac OS 8)

The project generates executables directly using LLVM 20, eliminating the need for intermediate formats.

### Repository Structure

```
llvm-project/
├── build/                           # LLVM build output
│   └── bin/                         # clang, ld.lld, llvm-readobj, etc.
├── lld/PEF/                         # PEF linker implementation (PowerPC)
│   ├── Writer.cpp                   # Core PEF generation (~2,600 lines)
│   ├── RelocWriter.cpp              # Relocation bytecode
│   └── Driver.cpp                   # CLI and library paths
├── lld/Classic68K/                  # Classic 68K linker implementation
│   ├── Writer.cpp                   # Core linking (~900 lines, 8-phase process)
│   ├── TrapStubs.cpp                # A-Trap stub generation
│   ├── TrapDatabase.cpp             # Mac Toolbox trap definitions
│   ├── SymbolTable.cpp              # Symbol resolution
│   ├── ELFReader.cpp                # ELF object file parsing
│   ├── DataSection.cpp              # A5 world data layout
│   └── Config.h                     # Configuration including verbose flag
├── llvm/include/llvm/BinaryFormat/PEF.h  # PEF data structures
├── llvm/lib/Object/PEFObjectFile.cpp     # PEF object file parser
├── clang/lib/Driver/ToolChains/       # Mac OS Classic driver
├── macos-classic/
│   ├── docs/                        # PEF format documentation
│   ├── test-programs/               # Test C/C++ source files
│   ├── resources/                   # CodeWarrior reference binaries [gitignored]
│   └── Interfaces&Libraries/        # MPW library stubs [gitignored]
├── scripts/                         # Build automation
│   ├── build_test.sh                # Compile + link + prepare
│   ├── build_with_runtime.sh        # Build with C runtime
│   ├── build_cxx_with_runtime.sh    # Build C++ with runtime
│   ├── relink_test.sh               # Relink only (after linker changes)
│   ├── extract_resources.sh         # Extract CODE/DATA from resource fork
│   ├── compare_llvm_vs_cw.sh        # Compare LLVM vs CodeWarrior binaries
│   ├── package-classic68k-disk.sh   # Create MFS disk image for emulators
│   ├── package-classic68k-app.sh    # Package app with resource fork
│   └── mfs_disk.py                  # MFS disk image creation (Python)
├── shared/                          # Mac OS 9-ready binaries [gitignored]
│   └── *_68k_cw                     # CodeWarrior 68K reference binaries
├── Makefile                         # Unified build system
└── CLAUDE.md                        # This file
```

Retro68 reference implementation is in the parent directory (`../Retro68/` and `../Retro68-build/`).

## Build Commands

### Using the Makefile

```bash
cd llvm-project

# Build specific LLVM components
make lld                    # Rebuild linker only (after Writer.cpp changes)
make clang                  # Rebuild compiler only
make runtime                # Rebuild Mac OS Classic runtime builtins
make macos-classic          # Rebuild both lld and runtime

# Full LLVM build
make llvm                   # Full ninja build (slow)

# Build test programs
make test NAME=minimal_test           # Build single test
make test-all                         # Build all tests

# Prepare for Mac OS 9
make prepare NAME=minimal_test        # Copy to shared/ with resource fork

# Clean
make clean-tests            # Remove test build artifacts
make clean-shared           # Remove shared directory contents
```

### Manual Build Commands

```bash
# Rebuild linker after changes to Writer.cpp
cd build && ninja lld

# Compile and link a test program
./scripts/build_test.sh minimal_test

# Relink only (after linker fixes)
./scripts/relink_test.sh minimal_test

# Build with C runtime support (uses main() instead of __start)
./scripts/build_with_runtime.sh beep_test_main
```

### Manual Compilation

```bash
# Compile to object file
./build/bin/clang --target=powerpc-apple-classic \
    -ffreestanding -nostdlib -nostdinc -c test.c -o test.o

# Link to PEF
./build/bin/ld.lld -flavor pef \
    -e __start test.o -lInterfaceLib -o test.pef

# Generate resource fork (cfrg resource required for Mac OS 9)
./scripts/generate_cfrg_resource.sh test.pef
```

### Inspecting PEF Binaries

```bash
# View PEF headers
./build/bin/llvm-readobj --pef-header binary.pef

# Disassemble
./build/bin/llvm-objdump --disassemble binary.pef

# Hexdump for debugging
hexdump -C binary.pef | head -n 10
```

## LLVM PEF Architecture

### Compilation Pipeline (Direct PEF Generation)

```
hello.c → clang (--target=powerpc-apple-classic) → hello.o (PEF object)
        → ld.lld (-flavor pef) → hello.pef (PEF executable)
        → Rez + resource fork → Mac OS 9 application
```

This differs from Retro68's approach which uses XCOFF as an intermediate: `GCC → XCOFF → MakePEF → PEF`

### Key LLD PEF Components (`lld/PEF/`)

- **Writer.cpp**: Core PEF generation - section layout, import stubs, TOC entries
- **RelocWriter.cpp**: PEF relocation bytecode generation
- **Driver.cpp**: Command-line interface, library search paths
- **InputFiles.cpp**: PEF object file loading

### Import Stub Architecture

LLVM uses 24-byte self-restoring import stubs (matching CodeWarrior):

```asm
lwz r12, offset(r2)     # Load import slot address from TOC
lwz r12, 0(r12)         # Dereference to get TVect
lwz r0, 0(r12)          # Load function code address
lwz r2, 4(r12)          # Load target library TOC
mtctr r0
bctr                    # Branch to function
```

### Data Section Layout (Critical)

```
[Import Address Table (4 bytes × N imports)]
[Entry Point TVect (12 bytes)]
[TOC Entries (12 bytes each)]
[User data]
```
The TVect must come BEFORE TOC entries so TVect.TOC points forward.

## PEF Format Key Points

- **Container Header**: 40 bytes, magic `Joy!peff`, architecture `pwpc`
- **Section Headers**: 28 bytes each (NOT 40!)
- **Loader Section**: TotalLength and UnpackedLength MUST be 0 (it's metadata, not loaded into memory)
- **Entry Point**: MainSection/MainOffset point to a TVect in data section (NOT code)
- **Relocations**: Bytecode format with 16-bit instructions (opcode:7 bits, operand:9 bits)

### Common Relocation Opcodes

- `0x20` BySectC: Add code section base
- `0x21` BySectD: Add data section base
- `0x23` TVector8: Patch 8-byte TVect
- `0x25` ImportRun: Patch import table slots

## Testing

### Static Validation

```bash
# Check headers
./build/bin/llvm-readobj --sections binary.pef

# Verify loader section TotalLength=0
hexdump -C -s 40 -n 120 binary.pef

# Look for Bug #28 corruption (0x01010400 in UnpackedLength = BAD)
```

### Runtime Testing (SheepShaver)

1. Build: `make test NAME=minimal_test`
2. Prepare: `make prepare NAME=minimal_test`
3. Test in SheepShaver Mac OS 9 emulator (shared/ is visible)

### Resource Fork Requirements

Mac OS 9 apps need a `cfrg` resource. The fragment name MUST match the binary filename.

```bash
./scripts/generate_cfrg_resource.sh output.pef "FragmentName"
```

## Important Implementation Details

### Fragment Name Must Match Binary Name

CFM locates code by matching the `cfrg` fragment name to the binary filename.

### Update Level Must Be 0

CodeWarrior-compatible binaries use update level 0, not 1.

### Import Stubs Use bctr Not bctrl

For noreturn functions like ExitToShell, use `bctr` (0x4E800420) not `bctrl`.

### TOC Pointer (r2)

Set to `data_section_base + TVect.TOCAddress` by CFM at load time.

## Documentation References

See `macos-classic/docs/`:

- `PEF_FORMAT_SPECIFICATION.md`: Complete PEF format reference
- `LLVM_PEF_ARCHITECTURE.md`: LLVM implementation deep dive
- `PEF_RELOCATIONS.md`: Relocation bytecode interpreter model
- `RETRO68_APPROACH.md`: How Retro68's XCOFF→PEF conversion works
- `PEF_IMPLEMENTATION_COMPARISON.md`: LLVM vs Retro68 comparison
- `TESTING_GUIDE.md`: Comprehensive testing procedures

### Apple Official Reference: Mac OS Runtime Architectures

The `macos-classic/docs/rt-architectures/` directory contains Apple's complete "Mac OS Runtime Architectures" book converted to Markdown. Key chapters for LLVM development:

- **Chapter 8: PEF Structure** (`08-pef-structure-*.md`) - Critical for linker implementation
  - Container/section header formats, loader section, relocation opcodes
- **Chapter 1: CFM-Based Runtime Architecture** (`01-cfm-based-*.md`) - How CFM loads executables
- **Chapter 2: Indirect Addressing** (`02-indirect-addressing-*.md`) - TOC and transition vectors
- **Chapter 4: PowerPC Runtime Conventions** (`04-powerpc-runtime-*.md`) - Calling conventions

See `rt-architectures/index.md` for the complete chapter list.

## Retro68 Reference Implementation

Retro68 (in `../Retro68/`) provides a working GCC-based implementation for comparison:

- Uses `powerpc-apple-macos-gcc` compiler
- Links to XCOFF format
- Converts via `MakePEF` tool
- Produces larger binaries (~6.2KB vs LLVM's ~3.3KB for minimal test)

Build Retro68 binaries for comparison:

```bash
../Retro68-build/toolchain/bin/powerpc-apple-macos-gcc -c test.c -o test.o
../Retro68-build/toolchain/bin/powerpc-apple-macos-ld test.o -o test.xcoff -lInterfaceLib
../Retro68-build/toolchain/bin/MakePEF test.xcoff -o test_retro68.pef
```

---

## Classic 68K Linker

The Classic 68K linker (`lld/Classic68K/`) generates executables for Motorola 68000 series Macs running System 6 through Mac OS 8.

### Compilation Pipeline

```
test.c → clang (--target=m68k-apple-classic) → test.o (ELF object)
       → ld.lld (-flavor classic68k) → test.pef (Classic 68K executable with resource fork)
```

### Manual Compilation

```bash
# Compile to ELF object file
./build/bin/clang --target=m68k-apple-classic \
    -ffreestanding -nostdlib -nostdinc -c test.c -o test.o

# Link to Classic 68K executable
./build/bin/ld.lld -flavor classic68k \
    -e __start test.o -o test.pef

# With verbose logging (for debugging)
./build/bin/ld.lld -flavor classic68k --verbose \
    -e __start test.o -o test.pef
```

### 8-Phase Linking Process

The linker processes code in 8 distinct phases (see `Writer.cpp`):

1. **Phase 1: Build Symbol Table** (lines 226-303)
   - Reads ELF object files
   - Collects defined symbols (code/data)
   - Records undefined references (external functions)

2. **Phase 2: Resolve Traps** (lines 306-315)
   - Maps undefined symbols to Mac Toolbox A-Traps via `TrapDatabase`
   - Determines calling convention: `TRAP_STACK` (inline) or `TRAP_REG_A0` (stub)
   - Records return type for Pascal calling convention

3. **Phase 3: Generate Trap Stubs** (lines 317-328)
   - Creates stub code for register-based traps (TRAP_REG_A0)
   - Stack-based traps are inlined directly

4. **Phase 4: Collect Code/Data** (lines 330-367)
   - Gathers .text, .data, .rodata, .bss sections

5. **Phase 5: Build Code Layout** (lines 396-411)
   - Layout: `[startup prologue] [trap stubs] [user code]`

6. **Phase 6: Apply Relocations** (lines 413-668) - **CRITICAL**
   - Patches JSR/BSR/BRA instructions
   - Inlines stack-based traps (replaces JSR with trap word)
   - Computes A5-relative offsets for data references

7. **Phase 7: Patch Startup Prologue** (lines 670-691)
   - Sets below-A5 size for A5 world initialization
   - Patches BSR to entry point

8. **Phase 8: Generate Resources** (lines 693-802)
   - Creates CODE 0 (jump table), CODE 1 (code), DATA 0 (globals), SIZE -1

### Verbose Debugging

Use `--verbose` flag to see detailed logging of all phases:

```bash
./build/bin/ld.lld -flavor classic68k --verbose -e __start -o output test.o
```

Sample verbose output:
```
=== Phase 1: Building Symbol Table ===
Processing 1 input file(s)
  Symbol: __start @ 0x0000 [code/func]
  Undefined references: 2
    -> SysBeep
    -> ExitToShell

=== Phase 2: Resolving Traps ===
  Resolved: ExitToShell -> 0xa9f4 (STACK, ret=void)
  Resolved: SysBeep -> 0xa9c8 (STACK, ret=void)

=== Phase 6: Processing Relocations ===
  Reloc #1 @ 0x000e R_68K_32 sym=SysBeep [TRAP 0xa9c8 STACK]
  Inline trap: SysBeep -> 0xa9c8
  Reloc #2 @ 0x001a R_68K_PC32 sym=ExitToShell [TRAP 0xa9f4 STACK]
  Inline trap (tail call): ExitToShell -> 0xa9f4
```

### A-Trap Inlining

For stack-based traps (most Toolbox calls), the linker inlines the trap directly:

**Before (object file):**
```asm
jsr     SysBeep         ; 4EB9 00000000 (6 bytes, relocation)
```

**After (linked):**
```asm
a9c8                    ; SysBeep trap word (2 bytes)
nop                     ; 4E71 (padding)
nop                     ; 4E71 (padding)
```

For traps that return values (Pascal calling convention):
```asm
a9c8                    ; Trap word
movea.l (sp)+, a0       ; 205F - pop pointer return value
nop                     ; Padding
```

### Resource Fork Extraction

Extract CODE/DATA resources from compiled binaries:

```bash
# Extract resources from a binary
./scripts/extract_resources.sh binary.pef /tmp/output

# Output files:
#   /tmp/output.rsrc      - Raw resource fork
#   /tmp/output.r         - DeRez text format
#   /tmp/output_CODE0.bin - Jump table
#   /tmp/output_CODE1.bin - Executable code
#   /tmp/output_DATA0.bin - Global data
```

### Disassembly

Use Retro68's m68k objdump for disassembly:

```bash
# Disassemble extracted CODE resource
../Retro68-build/toolchain/bin/m68k-apple-macos-objdump \
    -D -b binary -m m68k /tmp/output_CODE1.bin

# Disassemble ELF object with relocations
../Retro68-build/toolchain/bin/m68k-apple-macos-objdump \
    -d -r test.o
```

### Comparing LLVM vs CodeWarrior

CodeWarrior 68K reference binaries are in `shared/*_68k_cw`. Compare:

```bash
# Compare binaries
./scripts/compare_llvm_vs_cw.sh shared/simple_beep_68k_cw output.pef

# Manual comparison
./scripts/extract_resources.sh shared/simple_beep_68k_cw /tmp/cw
./scripts/extract_resources.sh output.pef /tmp/llvm
diff /tmp/cw_CODE1.bin /tmp/llvm_CODE1.bin
```

### Creating Disk Images for Emulators

To test in Classic Mac emulators, create a 400K MFS floppy disk image:

```bash
# Create disk image from compiled application
./scripts/package-classic68k-disk.sh myapp myapp.dsk

# Example with beep_simple_68k
cd shared
../scripts/package-classic68k-disk.sh beep_simple_68k beep_simple_68k.dsk
```

The script creates a 400K MFS (Macintosh File System) disk image compatible with System 1-6.

**Using the disk image:**

| Emulator | How to Use |
|----------|------------|
| Infinite Mac | Drag `.dsk` file to the emulator window |
| Mini vMac | File → Open Disk Image → select `.dsk` file |
| Basilisk II | Add to disk list in preferences |

**Full build-to-test workflow:**

```bash
# 1. Compile
./build/bin/clang --target=m68k-apple-classic \
    -ffreestanding -nostdlib -nostdinc -c test.c -o test.o

# 2. Link
./build/bin/ld.lld -flavor classic68k -e __start -o test test.o

# 3. Create disk image
./scripts/package-classic68k-disk.sh test test.dsk

# 4. Drag test.dsk to your emulator
```

### Memory Layout

Classic 68K uses A5-relative addressing for globals:

```
A5 World:
  [Below A5: Global data + BSS]  ← Negative offsets from A5
  A5 →
  [Above A5: Jump table]         ← Positive offsets from A5

CODE 1 Layout:
  0x0000-0x0049: Startup prologue (74 bytes)
  0x004a-...:    Trap stubs (if any TRAP_REG_A0)
  ...-end:       User code
```

### Known Issues

**Function Pointer Addressing Bug**: The Clang m68k backend generates A5-relative addressing for ALL symbols, including function pointers. However, Classic 68K uses:
- Data: A5-relative (correct)
- Code: Absolute from CODE segment base (compiler generates wrong code)

This causes function pointer tests to fail. The linker correctly calculates addresses, but the generated code incorrectly adds them to A5.

**Workaround**: Avoid taking addresses of functions for now. Direct calls work correctly.

### Relocation Types

| Type | Description | Linker Handling |
|------|-------------|-----------------|
| R_68K_32 | Absolute 32-bit | JSR abs.L → inline trap or BSR |
| R_68K_PC32 | PC-relative 32-bit | BRA.L/BSR.L → inline trap if TRAP_STACK |
| R_68K_PC16 | PC-relative 16-bit | Patch displacement |
| R_68K_16 | Absolute 16-bit | Direct patch |

### Trap Calling Conventions

| Convention | Description | Implementation |
|------------|-------------|----------------|
| TRAP_STACK | Parameters on stack (most traps) | Inline trap word |
| TRAP_REG_A0 | Parameter in A0 register | Generate stub |
| TRAP_DISPATCHER | Uses trap dispatcher | Generate stub |

### Trap Return Types

| Return Type | Pop Instruction | Used For |
|-------------|-----------------|----------|
| TRAP_RET_VOID | (none) | SysBeep, ExitToShell |
| TRAP_RET_POINTER | MOVEA.L (SP)+, A0 | GetResource, NewHandle |
| TRAP_RET_WORD | MOVE.W (SP)+, D0 | MemError, ResError |
| TRAP_RET_BYTE | MOVE.B (SP)+, D0 | Boolean returns |
