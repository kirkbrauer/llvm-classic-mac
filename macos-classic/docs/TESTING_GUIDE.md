# LLVM PEF Testing Guide

## Overview

This guide provides comprehensive testing procedures for validating LLVM-generated PEF binaries for Classic Mac OS. It covers both static analysis (structure validation) and dynamic testing (runtime execution).

**Target Platform:** Mac OS 9.x on PowerPC (SheepShaver emulator)
**Status:** Bug #28 fixed, ready for validation

---

## Prerequisites

### Required Tools

1. **LLVM Toolchain** (built from source)
   ```bash
   cd /Users/kirk/repos/toolchain-macos9/llvm-project/build
   ninja clang lld llvm-readobj llvm-objdump
   ```

2. **SheepShaver Emulator**
   - Download: https://sheepshaver.cebix.net/
   - Configured with Mac OS 9.2.1 or later
   - Serial/network access for file transfer

3. **Retro68 Toolchain** (for comparison)
   - Located: `/Users/kirk/repos/toolchain-macos9/Retro68-build/toolchain/bin/`

4. **Hexdump Utilities**
   - `hexdump` (macOS/Linux built-in)
   - `xxd` (alternative)

---

## Testing Workflow

```
┌──────────────────────┐
│ 1. Build Binary      │
│    (LLVM toolchain)  │
└──────────┬───────────┘
           │
           ↓
┌──────────────────────┐
│ 2. Static Analysis   │
│    (llvm-readobj)    │
└──────────┬───────────┘
           │
           ↓
┌──────────────────────┐
│ 3. Hexdump Check     │
│    (validate headers)│
└──────────┬───────────┘
           │
           ↓
┌──────────────────────┐
│ 4. Comparison Test   │
│    (vs Retro68)      │
└──────────┬───────────┘
           │
           ↓
┌──────────────────────┐
│ 5. Runtime Test      │
│    (SheepShaver)     │
└──────────────────────┘
```

---

## Phase 1: Build Binary

### Test Case 1: Minimal Executable

**Source:** `hello_minimal.c`

```c
// Minimal test: just exit
void ExitToShell(void);

int main(void) {
    ExitToShell();
    return 0;
}
```

**Build Commands:**

```bash
cd /Users/kirk/repos/toolchain-macos9

# Compile to object
llvm-project/build/bin/clang \
  --target=powerpc-apple-classic \
  -c hello_minimal.c \
  -o hello_minimal.o

# Link to PEF
llvm-project/build/bin/ld.lld \
  -flavor pef \
  hello_minimal.o \
  -L ./lib \
  -lInterfaceLib \
  -o hello_minimal.pef \
  --entry=_main
```

**Expected Output:**
```
File: hello_minimal.pef
Size: ~300-400 bytes
```

### Test Case 2: Multiple Imports

**Source:** `hello_multi.c`

```c
void ExitToShell(void);
void DebugStr(const char *str);

int main(void) {
    DebugStr("\pHello from LLVM PEF!");
    ExitToShell();
    return 0;
}
```

**Build:** (same as above with `hello_multi.c`)

**Expected Output:**
```
File: hello_multi.pef
Size: ~400-500 bytes (2 imports × 44-byte stubs)
```

---

## Phase 2: Static Analysis

### Using llvm-readobj

**Command:**
```bash
llvm-project/build/bin/llvm-readobj \
  --pef-header \
  --sections \
  --relocations \
  hello_minimal.pef
```

**Expected Output:**

```
PEF Container Header:
  Tag1: Joy! (0x4A6F7921)
  Tag2: peff (0x70656666)
  Architecture: pwpc (0x70777063)
  FormatVersion: 1
  DateTimeStamp: <timestamp>
  SectionCount: 3          ← Code, Data, Loader
  InstSectionCount: 2      ← Code, Data (loader not instantiated)

Section 0 (Code):
  TotalLength: 68
  UnpackedLength: 68
  ContainerLength: 68
  ContainerOffset: 0xA0
  SectionKind: 0 (Code)
  ShareKind: 4 (Global)
  Alignment: 4 (16 bytes)

Section 1 (Data):
  TotalLength: 32
  UnpackedLength: 32
  ContainerLength: 32
  ContainerOffset: 0xE4
  SectionKind: 1 (Unpacked Data)
  ShareKind: 1 (Process)
  Alignment: 4 (16 bytes)

Section 2 (Loader):
  TotalLength: 0           ← CRITICAL: Must be 0!
  UnpackedLength: 0        ← CRITICAL: Must be 0!
  ContainerLength: 144
  ContainerOffset: 0x104
  SectionKind: 4 (Loader)
  ShareKind: 4 (Global)
```

### Validation Checklist

- [ ] **Container Header**
  - [ ] Tag1 = 0x4A6F7921 ('Joy!')
  - [ ] Tag2 = 0x70656666 ('peff')
  - [ ] Architecture = 0x70777063 ('pwpc')
  - [ ] FormatVersion = 1
  - [ ] SectionCount = 3 (or correct value)
  - [ ] InstSectionCount = SectionCount - 1

- [ ] **Code Section**
  - [ ] TotalLength > 0
  - [ ] TotalLength = UnpackedLength = ContainerLength
  - [ ] SectionKind = 0
  - [ ] ShareKind = 4 (Global)
  - [ ] ContainerOffset valid (not 0xFFFFFFFF)

- [ ] **Data Section**
  - [ ] TotalLength > 0
  - [ ] TotalLength = UnpackedLength = ContainerLength
  - [ ] SectionKind = 1 (Unpacked Data)
  - [ ] ShareKind = 1 (Process)
  - [ ] No garbage values (not 0x01010400!)

- [ ] **Loader Section**
  - [ ] TotalLength = 0 ← CRITICAL!
  - [ ] UnpackedLength = 0 ← CRITICAL!
  - [ ] ContainerLength > 0
  - [ ] SectionKind = 4

---

## Phase 3: Hexdump Validation

### Container Header Check

**Command:**
```bash
hexdump -C hello_minimal.pef | head -n 3
```

**Expected Output:**
```
00000000  4a 6f 79 21 70 65 66 66  70 77 70 63 00 00 00 01  |Joy!peffpwpc....|
00000010  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00  |................|
00000020  00 03 00 02 00 00 00 00                           |........|
          ^^^^^ ^^^^^ ^^^^^^^^^^^^
          Sects Inst  Reserved
```

**Verify:**
- Bytes 0-3: `4a 6f 79 21` ('Joy!')
- Bytes 4-7: `70 65 66 66` ('peff')
- Bytes 8-11: `70 77 70 63` ('pwpc')
- Bytes 32-33: Section count (0x0003 = 3)
- Bytes 34-35: Inst section count (0x0002 = 2)

### Section Headers Check

**Command:**
```bash
hexdump -C -s 40 -n 120 hello_minimal.pef
```

**Expected Output:**
```
00000028  ff ff ff ff 00 00 00 00  00 00 00 44 00 00 00 44  |...........D...D|
          ^^^^^^^^^^^ ^^^^^^^^^^^  ^^^^^^^^^^^ ^^^^^^^^^^^
          NameOffset  DefAddr      TotalLen    UnpackLen
00000038  00 00 00 44 00 00 00 a0  00 04 00 00              |...D........|
          ^^^^^^^^^^^ ^^^^^^^^^^^  ^^^^^^^^^^^^
          ContLen     ContOff      Kind Share Align Res

[Next header at 0x50...]
00000050  ff ff ff ff 00 00 00 00  00 00 00 20 00 00 00 20  |........... ... |
[Loader header at 0x78...]
00000078  ff ff ff ff 00 00 00 00  00 00 00 00 00 00 00 00  |................|
                                    ^^^^^^^^^^^ ^^^^^^^^^^^
                                    Total=0     Unpacked=0 ← CRITICAL!
```

**Red Flags (Bug #28):**
- UnpackedLength = `01 01 04 00` (garbage!)
- ContainerLength = `ff ff ff ff` (invalid!)
- ContainerOffset = `00 00 00 00` (wrong!)

If you see these values, Bug #28 is NOT fixed!

### Data Section Layout Check

**Command:**
```bash
# Find data section offset from header (e.g., 0xE4)
hexdump -C -s 0xE4 -n 32 hello_minimal.pef
```

**Expected Layout:**
```
000000e4  00 00 00 00 00 00 00 04  00 00 00 00 00 00 00 10  |................|
          ^^^^^^^^^^^ ^^^^^^^^^^^  ^^^^^^^^^^^ ^^^^^^^^^^^
          Import[0]   TVect[0-3]   TVect[4-7]  TVect[8-11]
000000f4  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00  |................|
          ^^^^^^^^^^^              ^^^^^^^^^^^
          TOC[0-3]                 TOC[4-7]
```

**Verify:**
- Offset 0: Import table (4 bytes, zeros)
- Offset 4: TVect (12 bytes)
  - Word 0 (offset 4-7): Code address
  - Word 1 (offset 8-11): TOC address (should be 0x10)
  - Word 2 (offset 12-15): Environment (0)
- Offset 16: TOC entries (12 bytes each)

---

## Phase 4: Comparison Testing

### Build Retro68 Equivalent

**Source:** Same `hello_minimal.c`

```bash
# Compile with Retro68
Retro68-build/toolchain/bin/powerpc-apple-macos-gcc \
  -c hello_minimal.c -o hello_minimal_retro.o

# Link with Retro68
Retro68-build/toolchain/bin/powerpc-apple-macos-ld \
  hello_minimal_retro.o -o hello_minimal.xcoff \
  -lInterfaceLib

# Convert to PEF
Retro68-build/toolchain/bin/MakePEF \
  hello_minimal.xcoff -o hello_minimal_retro.pef
```

### Size Comparison

**Command:**
```bash
ls -lh hello_minimal*.pef
```

**Expected:**
```
-rw-r--r--  1 user  staff   332B  hello_minimal.pef       (LLVM)
-rw-r--r--  1 user  staff   6.2K  hello_minimal_retro.pef (Retro68)
```

**Analysis:**
- LLVM should be ~50% smaller than Retro68
- Both should be valid PEF files

### Structure Comparison

**Commands:**
```bash
# LLVM
llvm-readobj --pef-header hello_minimal.pef > llvm_headers.txt

# Retro68
llvm-readobj --pef-header hello_minimal_retro.pef > retro_headers.txt

# Compare
diff llvm_headers.txt retro_headers.txt
```

**Expected Differences:**
- File sizes (LLVM smaller)
- Section offsets (due to alignment)
- Loader section size (LLVM more compact)

**Similarities (Must Match):**
- Container header magic numbers
- Section counts
- Section kinds
- Loader header MainSection/MainOffset structure

---

## Phase 5: Runtime Testing (SheepShaver)

### Setup SheepShaver

1. **Download and Install:**
   - https://sheepshaver.cebix.net/
   - Configure with Mac OS 9.2.1 ROM

2. **Create Shared Folder:**
   - SheepShaver Preferences → Unix Shared Folder
   - Point to: `/Users/kirk/repos/toolchain-macos9/shared`

3. **Install Required Extensions:**
   - CarbonLib (if testing Carbon apps)
   - InterfaceLib (built-in)

### Transfer Binary

**Method 1: Shared Folder**
```bash
# Copy PEF to shared folder
cp hello_minimal.pef shared/

# In Mac OS 9:
# - Open "Unix" volume
# - Copy hello_minimal.pef to Desktop
```

**Method 2: Disk Image**
```bash
# Create HFS disk image
hformat -l "TestDisk" test.img 1440
hmount test.img
hcopy hello_minimal.pef :hello_minimal.pef
humount

# Mount in SheepShaver
```

### Add Resource Fork

**On Mac OS 9 (using Rez):**

**Create hello.r:**
```
#include <Types.r>
#include <CodeFragmentTypes.r>

resource 'cfrg' (0) {
    {
        kPowerPCCFragArch,
        kIsCompleteCFrag,
        kNoVersionNum,
        kNoVersionNum,
        0,
        0,
        kOnDiskFlat,
        kZeroOffset,
        kCFragGoesToEOF,
        "HelloApp",
        {
        }
    }
};

resource 'SIZE' (-1) {
    reserved,
    acceptSuspendResumeEvents,
    reserved,
    canBackground,
    doesActivateOnFGSwitch,
    backgroundAndForeground,
    dontGetFrontClicks,
    ignoreAppDiedEvents,
    is32BitCompatible,
    isHighLevelEventAware,
    localAndRemoteHLEvents,
    isStationeryAware,
    useTextEditServices,
    reserved,
    reserved,
    reserved,
    128 * 1024,  // Preferred memory
    128 * 1024   // Minimum memory
};
```

**Build Application:**
```bash
# On Mac OS 9:
Rez hello.r -o HelloApp -t APPL -c '????'

# Set data fork to PEF:
# Use ResEdit or:
setfile -d hello_minimal.pef HelloApp
```

### Test Execution

#### Test 1: Launch Check

**Procedure:**
1. Double-click HelloApp in Finder
2. Observe behavior

**Expected Results:**
- ✓ **Success:** App launches and immediately quits (ExitToShell called)
- ✗ **Failure 1:** "Application could not be found" (missing 'cfrg')
- ✗ **Failure 2:** Emulator crashes (Bug #28 not fixed!)
- ✗ **Failure 3:** Type 1/2 error (runtime crash)

**Debugging:**
- Check Console.log in Mac OS 9
- Look for CFM error messages
- Compare with Retro68 binary behavior

#### Test 2: Debugger Check

**Using MacsBug (on Mac OS 9):**

```
1. Install MacsBug
2. Launch app with Command-Power (drop to debugger)
3. Examine:
   - Register r2 (should point to data section)
   - Memory at entry point (should be TVect)
   - Code section (valid PowerPC instructions)

Useful Commands:
  G                     ; Go (continue execution)
  IL [address]          ; Disassemble
  DM [address]          ; Display memory
  DR                    ; Display registers
```

**Expected Registers:**
```
r2:  <data_base + TOC_offset>   (TOC pointer)
lr:  <return_address>            (from CFM)
sp:  <stack_pointer>             (valid stack)
pc:  <code_base + entry_offset>  (at entry point)
```

#### Test 3: Multiple Imports

**Source:** `hello_multi.c` (from Phase 1)

**Expected:**
- DebugStr shows dialog: "Hello from LLVM PEF!"
- App quits cleanly

**Failure Cases:**
- DebugStr crashes → Import stub broken
- Wrong message → Memory corruption
- Hang → Infinite loop in stub

---

## Automated Testing

### Shell Script: `test_pef.sh`

```bash
#!/bin/bash
# Automated PEF testing script

set -e

LLVM_BIN="/Users/kirk/repos/toolchain-macos9/llvm-project/build/bin"
TEST_DIR="/Users/kirk/repos/toolchain-macos9/test"

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m' # No Color

echo "=== LLVM PEF Testing Suite ==="
echo

# Test 1: Build minimal binary
echo "Test 1: Building minimal binary..."
$LLVM_BIN/clang --target=powerpc-apple-classic -c $TEST_DIR/hello_minimal.c -o /tmp/hello_minimal.o
$LLVM_BIN/ld.lld -flavor pef /tmp/hello_minimal.o -L./lib -lInterfaceLib -o /tmp/hello_minimal.pef --entry=_main

if [ -f /tmp/hello_minimal.pef ]; then
    echo -e "${GREEN}✓ Build succeeded${NC}"
else
    echo -e "${RED}✗ Build failed${NC}"
    exit 1
fi

# Test 2: Validate header
echo
echo "Test 2: Validating PEF header..."
MAGIC=$($LLVM_BIN/llvm-readobj --pef-header /tmp/hello_minimal.pef | grep "Tag1:" | awk '{print $2}')

if [ "$MAGIC" == "Joy!" ]; then
    echo -e "${GREEN}✓ Magic numbers correct${NC}"
else
    echo -e "${RED}✗ Invalid magic numbers: $MAGIC${NC}"
    exit 1
fi

# Test 3: Check loader section
echo
echo "Test 3: Checking loader section..."
TOTAL_LEN=$($LLVM_BIN/llvm-readobj --sections /tmp/hello_minimal.pef | grep -A 10 "Section 2 (Loader)" | grep "TotalLength:" | awk '{print $2}')

if [ "$TOTAL_LEN" == "0" ]; then
    echo -e "${GREEN}✓ Loader section correct (TotalLength=0)${NC}"
else
    echo -e "${RED}✗ Loader section wrong (TotalLength=$TOTAL_LEN)${NC}"
    exit 1
fi

# Test 4: Hexdump check for Bug #28
echo
echo "Test 4: Checking for Bug #28 corruption..."
# Read data section UnpackedLength (should NOT be 0x01010400)
UNPACKED=$(hexdump -s 92 -n 4 -e '4/1 "%02x"' /tmp/hello_minimal.pef)

if [ "$UNPACKED" == "01010400" ]; then
    echo -e "${RED}✗ Bug #28 detected! Section headers corrupted!${NC}"
    exit 1
else
    echo -e "${GREEN}✓ No corruption detected${NC}"
fi

# Test 5: Size check
echo
echo "Test 5: Binary size check..."
SIZE=$(stat -f%z /tmp/hello_minimal.pef)

if [ $SIZE -lt 500 ]; then
    echo -e "${GREEN}✓ Binary size reasonable: $SIZE bytes${NC}"
else
    echo -e "${RED}⚠ Binary larger than expected: $SIZE bytes${NC}"
fi

echo
echo "=== All Tests Passed ==="
```

**Usage:**
```bash
chmod +x test_pef.sh
./test_pef.sh
```

---

## Troubleshooting

### Problem: Emulator Crashes on Launch

**Possible Causes:**
1. Bug #28 (section header corruption)
2. Invalid relocation bytecode
3. Malformed loader section

**Diagnosis:**
```bash
# Check section headers
hexdump -C -s 40 -n 160 binary.pef

# Look for:
# - 0x01010400 in UnpackedLength (Bug #28)
# - 0xFFFFFFFF in ContainerLength
# - Misaligned offsets
```

**Solution:**
- Rebuild LLVM toolchain with Bug #28 fix
- Verify Writer.cpp changes applied

### Problem: Type 1/2 Error on Launch

**Possible Causes:**
1. Wrong entry point (not TVect)
2. Import stub broken
3. TOC pointer invalid

**Diagnosis:**
```bash
# Check MainSection/MainOffset
llvm-readobj --pef-header binary.pef | grep -A 2 "Loader"

# Verify points to data section with TVect
hexdump -C -s <MainOffset> -n 12 binary.pef
```

**Solution:**
- Ensure MainSection = 1 (data section)
- Ensure MainOffset points to TVect (after import table)

### Problem: "Application Could Not Be Found"

**Possible Causes:**
1. Missing 'cfrg' resource
2. Wrong file type/creator

**Diagnosis:**
```bash
# On Mac OS 9, use ResEdit:
# - Check for 'cfrg' resource (ID 0)
# - Check file type = 'APPL'
# - Check creator = '????'
```

**Solution:**
```bash
# Rebuild resource fork
Rez hello.r -o HelloApp -t APPL -c '????'
```

---

## Success Criteria

### Phase 1-3 (Static): ✓ PASS

- [ ] Binary builds without errors
- [ ] llvm-readobj shows valid headers
- [ ] Hexdump shows no corruption
- [ ] File size is 300-500 bytes
- [ ] Comparison with Retro68 shows structural similarity

### Phase 4 (SheepShaver): 🔄 TESTING

- [ ] Binary loads without crash
- [ ] App launches and quits cleanly
- [ ] Multiple imports work correctly
- [ ] No MacsBug errors

---

## Reporting Issues

### Bug Report Template

```markdown
**Bug Title:** [Brief description]

**Environment:**
- LLVM Version: [commit hash]
- Mac OS Version: 9.2.1
- Emulator: SheepShaver 2.5
- Host OS: macOS 14.x

**Steps to Reproduce:**
1. [Step 1]
2. [Step 2]
3. [Step 3]

**Expected Behavior:**
[What should happen]

**Actual Behavior:**
[What actually happens]

**Binary Analysis:**
```bash
# Hexdump output
hexdump -C binary.pef | head -n 20
```

**Crash Log:**
[SheepShaver crash log or Mac OS error]

**Comparison:**
[Does Retro68 binary work? Yes/No]
```

---

## References

- **PEF Specification:** [PEF_FORMAT_SPECIFICATION.md](PEF_FORMAT_SPECIFICATION.md)
- **LLVM Architecture:** [LLVM_PEF_ARCHITECTURE.md](LLVM_PEF_ARCHITECTURE.md)
- **Comparison:** [PEF_IMPLEMENTATION_COMPARISON.md](PEF_IMPLEMENTATION_COMPARISON.md)

---

**Document Version:** 1.0
**Last Updated:** 2025-11-13
**Author:** LLVM PEF Toolchain Project
