# PEF (Preferred Executable Format) Specification

## Overview

The **Preferred Executable Format (PEF)** is the native executable format for PowerPC-based Classic Mac OS (System 7 through Mac OS 9). It was designed by Apple for the **Code Fragment Manager (CFM)**, the dynamic linker/loader in Classic Mac OS.

**Key Characteristics:**
- Big-endian only
- PowerPC (pwpc) or 68K (m68k) architecture
- Compact relocation bytecode system
- Section-relative addressing
- Transition Vector (TVect) based entry points
- Designed for memory-constrained systems

**Official References:**
- Apple's "Mac OS Runtime Architectures" document
- Chapter 8: Complete PEF structure specification
- Available at: `developer.apple.com/library/archive/documentation/mac/pdf/MacOS_RT_Architectures.pdf`
- Online HTML version: https://preterhuman.net/macstuff/techpubs/mac/runtimehtml/RTArch-89.html

**IMPORTANT:** Section headers are **28 bytes**, not 40 bytes. Earlier versions of this document incorrectly stated 40 bytes.

---

## File Structure

A PEF file consists of:

```
┌─────────────────────────────────────────┐
│ Container Header (40 bytes)             │
├─────────────────────────────────────────┤
│ Section Header 0 (28 bytes)             │
│ Section Header 1 (28 bytes)             │
│ ...                                     │
│ Section Header N (28 bytes)             │
│ Loader Section Header (28 bytes)        │
├─────────────────────────────────────────┤
│ Section 0 Data (variable)               │
│ Section 1 Data (variable)               │
│ ...                                     │
│ Section N Data (variable)               │
├─────────────────────────────────────────┤
│ Loader Section Data (variable)          │
└─────────────────────────────────────────┘
```

---

## 1. Container Header

**Size:** 40 bytes
**Location:** Offset 0 in file

### Structure

```cpp
struct ContainerHeader {
  uint32_t Tag1;               // Offset 0:  'Joy!' (0x4A6F7921)
  uint32_t Tag2;               // Offset 4:  'peff' (0x70656666)
  uint32_t Architecture;       // Offset 8:  'pwpc' (0x70777063) for PowerPC
  uint32_t FormatVersion;      // Offset 12: Always 1
  uint32_t DateTimeStamp;      // Offset 16: Creation time (Mac epoch)
  uint32_t OldDefVersion;      // Offset 20: Version info
  uint32_t OldImpVersion;      // Offset 24: Version info
  uint32_t CurrentVersion;     // Offset 28: Version info
  uint16_t SectionCount;       // Offset 32: Total sections (including loader)
  uint16_t InstSectionCount;   // Offset 34: Sections loaded into memory
  uint32_t ReservedA;          // Offset 36: Must be 0
};
```

### Fields

**Tag1 and Tag2:** Magic numbers identifying PEF files
- Must be `0x4A6F7921` ('Joy!') and `0x70656666` ('peff')
- Any other values → CFM rejects the file

**Architecture:** Target CPU
- `0x70777063` ('pwpc') = PowerPC
- `0x6D36386B` ('m68k') = 68K

**FormatVersion:** Always 1
- No other versions exist

**SectionCount:** Total number of section headers
- Includes regular sections (code, data) AND loader section
- Example: 2 regular sections + 1 loader = 3

**InstSectionCount:** Sections instantiated in memory
- Does NOT include loader section (loader is metadata only)
- Example: 2 regular sections = 2

**Critical Rule:** `SectionCount = InstSectionCount + 1` (for typical executables)

---

## 2. Section Header

**Size:** 28 bytes per section
**Location:** Immediately after Container Header (starts at offset 0x28)

### Structure

```cpp
struct SectionHeader {
  int32_t  NameOffset;         // Offset 0:  -1 = unnamed
  uint32_t DefaultAddress;     // Offset 4:  0 = CFM chooses address
  uint32_t TotalLength;        // Offset 8:  Size in memory (includes BSS)
  uint32_t UnpackedLength;     // Offset 12: Size of initialized data
  uint32_t ContainerLength;    // Offset 16: Size in file
  uint32_t ContainerOffset;    // Offset 20: File offset to data
  uint8_t  SectionKind;        // Offset 24: Code/Data/Loader/etc
  uint8_t  ShareKind;          // Offset 25: Process/Global/Protected
  uint8_t  Alignment;          // Offset 26: Power of 2 (4 = 16 bytes)
  uint8_t  ReservedA;          // Offset 27: Must be 0
};
```

### Fields

**NameOffset:** Offset to section name in loader string table
- `-1` (0xFFFFFFFF) = unnamed section (typical for executables)

**DefaultAddress:** Preferred load address
- `0` = anywhere (CFM decides) → **Always use 0 for portability**

**TotalLength:** Total size in memory
- Includes uninitialized data (BSS)
- For pattern sections: unpacked size

**UnpackedLength:** Size of initialized data
- For code/data sections: same as TotalLength
- For pattern sections: may differ from TotalLength

**ContainerLength:** Size in file
- For unpacked sections: same as TotalLength
- For packed sections: compressed size
- For loader section: actual loader data size

**ContainerOffset:** File offset where section data begins
- Must be 16-byte aligned (typical)
- `0` for empty sections

**SectionKind:** Type of section
- `0` = Code (kPEFCodeSection)
- `1` = Unpacked Data (kPEFUnpackedDataSection)
- `2` = Pattern Data (kPEFPatternDataSection)
- `3` = Constant (kPEFConstantSection)
- `4` = Loader (kPEFLoaderSection)
- `5` = Debug (kPEFDebugSection)

**ShareKind:** Memory sharing mode
- `1` = Process (kPEFProcessShare) - separate copy per process
- `4` = Global (kPEFGlobalShare) - shared across processes
- `5` = Protected (kPEFProtectedShare) - read-only global

**Alignment:** Log2 of alignment
- `4` = 16 bytes (2^4)
- `0` = 1 byte (no alignment)

### Loader Section Header (CRITICAL!)

**The loader section is special:**

```cpp
// CORRECT loader section header:
TotalLength = 0;           // NOT loaded into memory!
UnpackedLength = 0;        // NOT unpacked!
ContainerLength = <actual size>;  // Size in file
SectionKind = 4;           // kPEFLoaderSection
```

**Why TotalLength = 0?**
- Loader section contains metadata for CFM
- It's read from file but NOT loaded into process memory
- Setting TotalLength ≠ 0 confuses CFM and causes crashes

---

## 3. Section Types

### Code Section (SectionKind = 0)

**Contains:** Executable PowerPC instructions

**Typical Layout:**
```
┌──────────────────────────────┐
│ User code (from .text)       │
├──────────────────────────────┤
│ Import stubs (44 bytes each) │
└──────────────────────────────┘
```

**ShareKind:** Global (4) - code is shared across processes
**Alignment:** 16 bytes (typical)

### Data Section (SectionKind = 1 or 2)

**Contains:** Initialized global variables, TVects, TOC entries

**CRITICAL Layout (for imports):**
```
┌───────────────────────────────────┐  ← Offset 0
│ Import Address Table              │  4 bytes × N imports
│ (zeros - CFM patches at load)     │
├───────────────────────────────────┤  ← Offset 4N (if N=1, offset 4)
│ Entry Point TVect (12 bytes)      │  TVect for main()
├───────────────────────────────────┤  ← Offset 4N+12 (if N=1, offset 16)
│ TOC Entries (12 bytes each)       │  One per import
│ Each points to import table slot  │
├───────────────────────────────────┤
│ User data (from .data, .bss)      │
└───────────────────────────────────┘
```

**Why This Order Matters:**
1. Import table at offset 0 (CFM expectation)
2. TVect before TOC entries (so TVect.TOC can point forward)
3. TOC entries use positive offsets from r2

**ShareKind:** Process (1) - separate copy per process
**Alignment:** 16 bytes (typical)

### Pattern-Initialized Data Section (SectionKind = 2)

**Purpose:** Space-efficient encoding of initialized data with repeating patterns

Pattern-initialized sections use a bytecode instruction set to encode repetitive data patterns, allowing significant file size reduction compared to storing raw data.

**Reference:** Apple's "Mac OS Runtime Architectures", Chapter 8
https://preterhuman.net/macstuff/techpubs/mac/runtimehtml/RTArch-94.html#HEADING94-18

#### Section Header for Pattern-Init

```cpp
SectionKind = 2;                      // kPEFPatternDataSection
TotalLength = <final size in memory>; // e.g., 16 bytes
UnpackedLength = <pattern data size>; // e.g., 12 bytes (excludes BSS)
ContainerLength = <packed size>;      // e.g., 1 byte (just the opcodes)
```

**Example:**
- File contains: 1 byte of pattern opcodes
- CFM unpacks to: 12 bytes of data in memory
- Section also has: 4 bytes of BSS (zero-initialized)
- Total memory: 16 bytes

#### Pattern-Init Opcodes

Each instruction is 1 byte with the format:
```
Bits 7-5: Opcode (3 bits)
Bits 4-0: Count or parameter (5 bits)
```

**Opcode 000: Zero (zero-fill)**
- Initializes Count bytes to 0x00
- If Count field = 0: Next bytes specify extended count
- Example: `0x0C` = `0b00001100` = Zero-fill 12 bytes

**Opcode 001: BlockCopy (copy literal data)**
- Copies blockSize bytes of literal data following the instruction
- Literal data bytes follow immediately after the instruction

**Opcode 010: RepeatedBlock (repeat pattern)**
- Repeats a block pattern multiple times
- blockSize bytes follow, then repeat (repeatCount-1) more times

**Opcode 011: InterleaveRepeatBlockWithBlockCopy**
- Alternates between common pattern and custom data
- Pattern: [common][custom][common][custom]...[common]

**Opcode 100: InterleaveRepeatBlockWithZero**
- Alternates between common pattern and zero bytes
- Pattern: [common][zeros][common][zeros]...[common]

#### Multi-Byte Arguments

When count/size exceeds 31, use multi-byte encoding:
- Set 5-bit field to 0
- Follow with variable-length argument:
  - Bit 7 = 1: More bytes follow
  - Bits 6-0: Data bits
  - Big-endian order

**Example:** 300 bytes = `0b100101100`
```
Byte 1: 0x82  (0b10000010 - bit 7 set, bits 0-6 = 0000010)
Byte 2: 0x2C  (0b00101100 - bit 7 clear, bits 0-6 = 0101100)
Result: 0000010_0101100 = 0b100101100 = 300
```

#### CodeWarrior Example

The CodeWarrior binary uses pattern-init for the data section:

**Section Header:**
```
TotalLength:     0x00000010 (16 bytes in memory)
UnpackedLength:  0x0000000C (12 bytes pattern-initialized)
ContainerLength: 0x00000001 (1 byte in file)
SectionKind:     0x02        (Pattern-init)
```

**Pattern Data (1 byte):**
```
0x0C = 0b00001100
  Bits 7-5: 000 (Zero opcode)
  Bits 4-0: 01100 (count = 12)

Meaning: Zero-fill 12 bytes
```

**Result in Memory:**
```
0x00-0x03: [00 00 00 00]  ← Import table (4 bytes)
0x04-0x0F: [00 00 00 00 00 00 00 00 00 00 00 00]  ← Pattern data (12 zeros)
0x10-0x13: [00 00 00 00]  ← BSS (4 bytes, TotalLength - UnpackedLength)
```

The entry point TVect at offset 0x04 is within this zero-filled region.

#### When to Use Pattern-Init

**Use pattern-init when:**
- Data has repeating patterns (common in initialized globals)
- Large zero-initialized regions (common + efficient)
- Alternating common/custom data structures

**Use unpacked data when:**
- Data is non-repetitive
- File size is not a concern
- Simplicity is preferred

**Size comparison for 12-byte zero region:**
- Unpacked: 12 bytes in file
- Pattern-init: 1 byte in file (92% reduction)

#### Implementation Notes

LLVM currently uses unpacked data (SectionKind=1) for simplicity. Implementing pattern-init encoding would require:
1. Analyzing data section contents for patterns
2. Encoding patterns using opcode instructions
3. Setting appropriate header fields (ContainerLength, UnpackedLength)
4. Potential file size savings: ~27 bytes for minimal_test (~8%)

This is a **future optimization** - unpacked data works correctly but uses more space.

---

## 4. Transition Vectors (TVects)

### Concept

**Entry points in PEF are descriptors, NOT code addresses.**

A TVect is a 12-byte structure that describes how to call a function:

```cpp
struct TransitionVector {
  uint32_t CodeAddress;     // Offset to code within code section
  uint32_t TOCAddress;      // TOC pointer (r2 value) for this fragment
  uint32_t Environment;     // 0 for executables, varies for shared libs
};
```

### How CFM Uses TVects

When CFM launches an executable:

1. Read `MainSection` and `MainOffset` from loader header
2. Locate the TVect at that section:offset
3. Read the 3 words from the TVect
4. Set `r2 = data_section_base + TVect.TOCAddress`
5. Jump to `code_section_base + TVect.CodeAddress`

**Example:**

```
Entry Point TVect (in data section at offset 4):
  [0-3]:   0x00000000    ; Code offset 0
  [4-7]:   0x00000010    ; TOC offset 16 (after import table + TVect)
  [8-11]:  0x00000000    ; Environment 0

CFM execution:
  r2 = data_base + 0x10   ; Points to TOC entries
  pc = code_base + 0x00   ; Jump to main
```

### TVect Placement

**Must be in data section** (NOT code section)
- Typically after import address table
- Before TOC entries

**Loader header points to TVect:**
```cpp
loaderInfoHeader.MainSection = 1;    // Data section index
loaderInfoHeader.MainOffset = 4;      // Offset of TVect in data
```

---

## 5. Relocation System

PEF uses a **compact bytecode** format for relocations to minimize file size.

### Relocation Instructions

**Format:** 16-bit instructions
**Encoding:** `[opcode:7 bits][operand:9 bits]`

```
Bit layout:
  15 14 13 12 11 10  9  8  7  6  5  4  3  2  1  0
 ┌──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┬──┐
 │  Opcode (7 bits)   │     Operand (9 bits)     │
 └──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┴──┘
```

**Extraction:**
```cpp
uint8_t opcode = (instr >> 9) & 0x7F;
uint16_t operand = instr & 0x1FF;
```

### Common Opcodes

**BySectC (0x20):** Add code section base
```
Relocates 'count' words by adding code section base address
Count = operand + 1
Advances cursor by 4 × count bytes
```

**BySectD (0x21):** Add data section base
```
Relocates 'count' words by adding data section base address
Count = operand + 1
Advances cursor by 4 × count bytes
```

**TVector8 (0x23):** Patch 8-byte TVect
```
Relocates two words of a TVect:
  Word 0 (CodeAddress): += code section base
  Word 1 (TOCAddress): += data section base
  Word 2 (Environment): unchanged
Count = operand + 1 (number of TVects)
Advances cursor by 8 × count bytes
```

**ImportRun (0x25):** Patch import table
```
Relocates 'count' import table slots
CFM patches these with actual TVect addresses at load time
Count = operand + 1
Advances cursor by 4 × count bytes
```

**SetPosition (0x48):** Set relocation cursor
```
Two-instruction sequence:
  Instr 1: 0x48xx (high 9 bits)
  Instr 2: 0xYYYY (low 16 bits)
  Position = (xx << 16) | YYYY
Sets cursor to absolute offset within section
```

**LgByImport (0x52):** Import by index
```
Two-instruction sequence:
  Instr 1: 0x52xx (high 9 bits of import index)
  Instr 2: 0xYYYY (low 16 bits of import index)
  Index = (xx << 16) | YYYY
Relocates 1 word by adding import's TVect address
Advances cursor by 4 bytes
```

### Relocation Example

**Minimal executable with 1 import:**

```
Relocation header (12 bytes):
  SectionIndex: 1        ; Data section
  RelocCount: 2          ; 2 instructions
  FirstRelocOffset: 0    ; Start at offset 0

Relocation instructions (4 bytes):
  0x4A00   ; ImportRun, count=1 (patch 1 import slot)
  0x4600   ; TVector8, count=1 (patch 1 TVect)

Execution:
  Cursor = 0

  ImportRun (count=1):
    - Mark offset 0-3 as import slot
    - Cursor advances to 4

  TVector8 (count=1):
    - Word at offset 4-7: += code_base (CodeAddress)
    - Word at offset 8-11: += data_base (TOCAddress)
    - Cursor advances to 12
```

**Result:**
- Import table slot at offset 0 marked for CFM patching
- TVect at offset 4-15 properly relocated

---

## 6. Loader Section

**Purpose:** Contains metadata for CFM
**NOT loaded into memory** (TotalLength = 0)

### Loader Section Layout

```
┌─────────────────────────────────────┐
│ Loader Info Header (56 bytes)       │
├─────────────────────────────────────┤
│ Imported Libraries (24 bytes each)  │
├─────────────────────────────────────┤
│ Imported Symbols (4 bytes each)     │
├─────────────────────────────────────┤
│ Relocation Headers (12 bytes each)  │
├─────────────────────────────────────┤
│ Relocation Instructions (variable)  │
├─────────────────────────────────────┤
│ Loader Strings (null-terminated)    │
├─────────────────────────────────────┤
│ Export Hash Table (variable)        │
├─────────────────────────────────────┤
│ Export Key Table (4 bytes × exports)│
├─────────────────────────────────────┤
│ Exported Symbols (10 bytes each)    │
└─────────────────────────────────────┘
```

### Loader Info Header

**Size:** 56 bytes

```cpp
struct LoaderInfoHeader {
  int32_t  MainSection;               // Offset 0:  Section containing main TVect
  uint32_t MainOffset;                // Offset 4:  Offset of main TVect
  int32_t  InitSection;               // Offset 8:  Init function section (-1 = none)
  uint32_t InitOffset;                // Offset 12: Init function offset
  int32_t  TermSection;               // Offset 16: Term function section (-1 = none)
  uint32_t TermOffset;                // Offset 20: Term function offset
  uint32_t ImportedLibraryCount;      // Offset 24: Number of imported libraries
  uint32_t TotalImportedSymbolCount;  // Offset 28: Total imported symbols
  uint32_t RelocSectionCount;         // Offset 32: Number of relocation sections
  uint32_t RelocInstrOffset;          // Offset 36: Offset to relocation instructions
  uint32_t LoaderStringsOffset;       // Offset 40: Offset to string table
  uint32_t ExportHashOffset;          // Offset 44: Offset to export hash table
  uint32_t ExportHashTablePower;      // Offset 48: Hash table size = 2^N
  uint32_t ExportedSymbolCount;       // Offset 52: Number of exported symbols
};
```

**Critical Fields:**

**MainSection/MainOffset:** Points to entry point TVect
- MUST point to a TVect in data section
- NOT a code address!
- Example: `MainSection=1, MainOffset=4` → TVect at data[4:15]

**RelocInstrOffset:** Points to relocation **instructions**
- NOT the headers!
- Offset is relative to start of loader section
- Instructions come after headers

**ExportHashTablePower:** Hash table size
- Executables: typically 1 (2^1 = 2 slots)
- Libraries: larger for many exports

### Imported Library Entry

**Size:** 24 bytes

```cpp
struct ImportedLibrary {
  uint32_t NameOffset;           // Offset to library name in string table
  uint32_t OldImpVersion;        // Old implementation version
  uint32_t CurrentVersion;       // Current version
  uint32_t ImportedSymbolCount;  // Symbols from this library
  uint32_t FirstImportedSymbol;  // Index of first symbol
  uint8_t  Options;              // 0 = strong, 1 = weak
  uint8_t  ReservedA;
  uint16_t ReservedB;
};
```

**Example:**
```
Library: InterfaceLib
  NameOffset: 0 (string at loaderStrings[0])
  ImportedSymbolCount: 1 (ExitToShell)
  FirstImportedSymbol: 0 (global index)
```

### Imported Symbol Entry

**Size:** 4 bytes

```cpp
struct ImportedSymbol {
  uint32_t ClassAndName;  // [class:4 bits][nameOffset:28 bits]
};
```

**Symbol Classes:**
- `0x0` = Code
- `0x1` = Data
- `0x2` = TVect (most common for function imports)
- `0x3` = TOC
- `0x4` = Glue

**Encoding:**
```cpp
uint8_t symbolClass = (ClassAndName >> 24) & 0x0F;
uint32_t nameOffset = ClassAndName & 0x00FFFFFF;
```

**Example:**
```
Import: ExitToShell
  ClassAndName: 0x02000010
  symbolClass: 0x2 (TVect)
  nameOffset: 0x10 (string at loaderStrings[16])
```

### Relocation Header

**Size:** 12 bytes

```cpp
struct LoaderRelocationHeader {
  uint16_t SectionIndex;       // Section to relocate
  uint16_t RelocCount;         // Number of reloc instructions
  uint32_t FirstRelocOffset;   // Offset within section to start
  uint32_t ReservedA;
};
```

---

## 7. CFM Loading Process

### At Load Time

1. **Parse Container Header**
   - Verify magic numbers ('Joy!peff')
   - Check architecture (pwpc)
   - Read section count

2. **Load Sections**
   - Read section headers
   - Allocate memory for each section (TotalLength bytes)
   - Read section data from file (ContainerLength bytes)
   - Skip loader section (TotalLength = 0)

3. **Resolve Imports**
   - For each imported library:
     - Find library fragment in system
     - For each imported symbol:
       - Look up symbol in library's exports
       - Get symbol's TVect address
       - Write TVect address to import table slot

4. **Apply Relocations**
   - For each relocation header:
     - Set cursor to FirstRelocOffset
     - For each relocation instruction:
       - Apply relocation (add section base, patch import, etc.)
       - Advance cursor

5. **Call Initializers**
   - If InitSection ≠ -1:
     - Call function at InitSection:InitOffset

6. **Jump to Entry Point**
   - Read TVect at MainSection:MainOffset
   - Set r2 = data_base + TVect.TOCAddress
   - Call code_base + TVect.CodeAddress

### At Runtime

**Import Call Sequence:**

```
User code:
  bl import_stub_ExitToShell

Import stub (44 bytes):
  mflr r11                   ; Save return address
  lwz r12, 0(r2)             ; Load TOC entry → import slot address
  lwz r12, 0(r12)            ; Dereference → ExitToShell TVect address
  stw r2, 20(r1)             ; Save our TOC
  lwz r0, 0(r12)             ; Load ExitToShell code address
  lwz r2, 4(r12)             ; Load InterfaceLib TOC
  mtctr r0                   ; Prepare call
  bctrl                      ; Call ExitToShell
  lwz r2, 20(r1)             ; Restore our TOC
  mtlr r11                   ; Restore return address
  blr                        ; Return to user code
```

**Key Points:**
- r2 must be saved/restored across fragment calls
- Import stubs handle TOC switching automatically
- Double-dereference: TOC entry → import slot → TVect → function

---

## 8. Common Pitfalls

### Crash-Causing Errors

**These cause emulator/OS crashes:**

1. **Invalid Loader Section Header**
   ```cpp
   // WRONG:
   loaderSectionHeader.TotalLength = loaderData.size();

   // CORRECT:
   loaderSectionHeader.TotalLength = 0;
   loaderSectionHeader.UnpackedLength = 0;
   loaderSectionHeader.ContainerLength = loaderData.size();
   ```

2. **Section Header Corruption**
   ```cpp
   // WRONG: Skipping empty sections without advancing buffer
   if (section.isEmpty())
     continue;  // Buffer pointer not advanced!
   write_header(buf, section);
   buf += sizeof(SectionHeader);

   // CORRECT: Always advance buffer
   write_header(buf, section);
   buf += sizeof(SectionHeader);  // Even for empty sections
   ```

3. **Misaligned Sections**
   ```cpp
   // File offsets must be aligned
   offset = alignTo(offset, 16);
   section.setFileOffset(offset);
   ```

4. **Invalid Relocation Opcodes**
   ```cpp
   // WRONG: Wrong bit shift
   uint8_t opcode = (instr >> 10) & 0x3F;

   // CORRECT:
   uint8_t opcode = (instr >> 9) & 0x7F;
   ```

### Runtime-Failure Errors

**These cause app failures (but not crashes):**

1. **Wrong Entry Point Type**
   ```cpp
   // WRONG: Pointing to code
   loaderInfoHeader.MainSection = 0;  // Code section
   loaderInfoHeader.MainOffset = codeOffset;

   // CORRECT: Pointing to TVect
   loaderInfoHeader.MainSection = 1;  // Data section
   loaderInfoHeader.MainOffset = tvectOffset;
   ```

2. **Wrong Data Section Layout**
   ```cpp
   // WRONG: [Import][TOC][TVect]
   // TVect.TOC points backward!

   // CORRECT: [Import][TVect][TOC]
   // TVect.TOC points forward
   ```

3. **Wrong TOC Base**
   ```cpp
   // WRONG: r2 points to middle of data section
   uint32_t tocBase = dataSize / 2;

   // CORRECT: r2 points to TOC entries
   uint32_t tocBase = tocEntriesOffset;
   ```

---

## 9. Validation Checklist

### Container Header
- [ ] Tag1 = 0x4A6F7921 ('Joy!')
- [ ] Tag2 = 0x70656666 ('peff')
- [ ] Architecture = 0x70777063 ('pwpc')
- [ ] FormatVersion = 1
- [ ] SectionCount matches actual headers written
- [ ] InstSectionCount = SectionCount - 1

### Section Headers
- [ ] All section kinds are valid (0-5)
- [ ] Loader section: TotalLength = 0, UnpackedLength = 0
- [ ] File offsets are 16-byte aligned
- [ ] No overlapping sections
- [ ] ContainerLength ≤ file size

### Data Section
- [ ] Layout: [Import][TVect][TOC][User Data]
- [ ] TVect is 12 bytes
- [ ] TVect.TOCAddress points to TOC entries
- [ ] Import table is zeros (CFM patches)

### Loader Section
- [ ] MainSection points to data section
- [ ] MainOffset points to TVect (not code!)
- [ ] RelocInstrOffset points past relocation headers
- [ ] All string offsets are valid

### Relocations
- [ ] Opcodes are valid (< 0x80)
- [ ] TVector8 patches 8 bytes (not 12!)
- [ ] ImportRun patches import table
- [ ] Cursor doesn't go out of bounds

---

## 10. Tools

### llvm-readobj
```bash
llvm-readobj --pef-header myapp.pef
llvm-readobj --sections myapp.pef
llvm-readobj --relocations myapp.pef
```

### llvm-objdump
```bash
llvm-objdump --pef-dump myapp.pef
llvm-objdump --disassemble myapp.pef
```

### hexdump
```bash
# View container header
hexdump -C myapp.pef | head -n 3

# View section headers
hexdump -C -s 40 -n 120 myapp.pef

# View loader section
hexdump -C -s <loader_offset> -n 56 myapp.pef
```

---

## 11. References

1. **Apple Documentation**
   - "Mac OS Runtime Architectures" PDF
   - Chapter 8: PEF Specification

2. **Implementation Examples**
   - Retro68: `Retro68/PEFTools/MakePEF.cc`
   - LLVM: `llvm-project/lld/PEF/Writer.cpp`

3. **File Format Headers**
   - LLVM: `llvm-project/llvm/include/llvm/BinaryFormat/PEF.h`
   - Retro68: `Retro68/PEFTools/PEFBinaryFormat.h`

---

## Appendix A: Quick Reference

### Magic Numbers
- Container Tag1: `0x4A6F7921` ('Joy!')
- Container Tag2: `0x70656666` ('peff')
- Architecture: `0x70777063` ('pwpc')

### Section Kinds
- 0 = Code
- 1 = Unpacked Data
- 2 = Pattern Data
- 3 = Constant
- 4 = Loader
- 5 = Debug

### Share Kinds
- 1 = Process
- 4 = Global
- 5 = Protected

### Symbol Classes
- 0x0 = Code
- 0x1 = Data
- 0x2 = TVect
- 0x3 = TOC
- 0x4 = Glue

### Common Relocation Opcodes
- 0x20 = BySectC
- 0x21 = BySectD
- 0x23 = TVector8
- 0x25 = ImportRun
- 0x48 = SetPosition
- 0x52 = LgByImport

---

**Document Version:** 1.0
**Last Updated:** 2025-11-13
**Author:** LLVM PEF Toolchain Project
