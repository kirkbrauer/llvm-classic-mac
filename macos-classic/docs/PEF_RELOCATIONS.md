# PEF Relocation Format

## Overview

The Preferred Executable Format (PEF) uses a **streaming bytecode interpreter** model for relocations. Unlike traditional object formats (ELF, COFF) that store explicit "patch location X with symbol Y" entries, PEF uses a pseudo-microprocessor that processes a stream of 16-bit instructions to transform section data.

## Reference

Apple's official documentation: [PEF Relocation Instructions](https://preterhuman.net/macstuff/techpubs/mac/runtimehtml/RTArch-98.html#HEADING98-0)

## Streaming Bytecode Model

### State Machine

The relocation processor maintains four state variables:

1. **relocAddress**: Current location being relocated (offset within section)
2. **importIndex**: Current import symbol index (increments as imports are processed)
3. **sectionC**: Base address of code section (typically section 0)
4. **sectionD**: Base address of data section (typically section 1)

### Execution Flow

```
Initialize:
  relocAddress = 0
  importIndex = 0
  sectionC = codeSection.baseAddress
  sectionD = dataSection.baseAddress

For each 16-bit relocation instruction:
  1. Decode opcode (bits 15-9, 7 bits)
  2. Decode operand (bits 8-0, 9 bits)
  3. Execute instruction (may consume additional words)
  4. Update state variables
  5. Patch memory at relocAddress
```

## Instruction Encoding

Each instruction is a 16-bit big-endian value:

```
Bits:  15 14 13 12 11 10  9  8  7  6  5  4  3  2  1  0
       [  Opcode (7 bits) ][ Operand (9 bits)        ]
```

Some instructions use **two consecutive 16-bit words** for larger operands (up to 25 bits total).

## Complete Opcode Reference

### Basic Relocations (0x00-0x1F)

#### 0x00 - RelocBySectDWithSkip
**Format**: `[skipCount:5][relocCount:11]`

**Purpose**: Skip some words, then add sectionD base to next N words

**Encoding**:
- Bits 15-11: Skip count (0-31 words)
- Bits 10-0: Relocation count (0-2047 words)

**Example**:
```
Instruction: 0x0042
  skipCount = 0
  relocCount = 66
Action:
  for i in 0..66:
    memory[relocAddress] += sectionD
    relocAddress += 4
```

### Section-Relative Relocations (0x20-0x2F)

#### 0x20 - RelocBySectC
**Format**: `[opcode:7][runLength-1:9]`

**Purpose**: Add code section base address to N consecutive words

**Example**:
```
Instruction: 0x4001  // 0x20 << 9 | 1
  runLength = 2
Action:
  for i in 0..2:
    memory[relocAddress] += sectionC
    relocAddress += 4
```

**Common usage**: Patching function pointers, vtables, code references

#### 0x21 - RelocBySectD
**Format**: `[opcode:7][runLength-1:9]`

**Purpose**: Add data section base address to N consecutive words

**Example**:
```
Instruction: 0x4200  // 0x21 << 9 | 0
  runLength = 1
Action:
  memory[relocAddress] += sectionD
  relocAddress += 4
```

**Common usage**: Patching data pointers, global variable references

#### 0x22 - RelocTVector12
**Format**: `[opcode:7][count-1:9]`

**Purpose**: Patch N transition vectors (12 bytes each)

**Structure of TVect**:
- Bytes 0-3: Code address (add sectionC)
- Bytes 4-7: TOC address (add sectionD)
- Bytes 8-11: Reserved (no relocation)

**Example**:
```
Instruction: 0x4400  // Patch 1 TVect
Action:
  memory[relocAddress + 0] += sectionC  // Code pointer
  memory[relocAddress + 4] += sectionD  // TOC pointer
  // Skip bytes 8-11
  relocAddress += 12
```

#### 0x23 - RelocTVector8
**Format**: `[opcode:7][count-1:9]`

**Purpose**: Patch N transition vectors (8-byte compact format)

**Structure**: Same as TVector12 but without reserved word

**Example**:
```
Instruction: 0x4600  // Patch 1 TVect (CodeWarrior standard for main entry)
Action:
  memory[relocAddress + 0] += sectionC
  memory[relocAddress + 4] += sectionD
  relocAddress += 8
```

**Critical use**: CodeWarrior linker uses this for the main application TVect

#### 0x24 - RelocVTable8
**Format**: `[opcode:7][count-1:9]`

**Purpose**: Patch C++ virtual table entries (8 bytes each)

**Structure**:
- Bytes 0-3: Virtual function address (add sectionC)
- Bytes 4-7: TOC value (add sectionD)

### Import Relocations (0x25-0x27)

#### 0x25 - RelocImportRun
**Format**: `[opcode:7][count-1:9]`

**Purpose**: Patch N **consecutive** import slots with symbol addresses

**Example**:
```
Instruction: 0x4A02  // 0x25 << 9 | 2
  count = 3
Action:
  for i in 0..3:
    memory[relocAddress] = importSymbols[importIndex].tvectAddress
    relocAddress += 4
    importIndex += 1
```

**Critical**: This is how CFM patches the import address table! Each slot gets filled with the transition vector address of an imported symbol.

**CodeWarrior usage**:
```
Data section layout:
  [Import table (4*N bytes)][Main TVect (12 bytes)]

Relocations:
  ImportRun(count=N)  // Patch all N import slots starting at offset 0
  TVector8            // Patch main TVect at offset N*4
```

### Position Control (0x28-0x2F)

#### 0x28 - RelocSmRepeat
**Format**: `[opcode:7][count-1:9]`

**Purpose**: Repeat last relocation operation N times

**Example**:
```
Previous: BySectC(runLength=1)  // Patched 1 word
Current:  SmRepeat(count=4)     // Repeat 4 more times
Result:   5 words total patched with sectionC
```

#### 0x29 - RelocSmSetSectC
**Format**: `[opcode:7][index:9]`

**Purpose**: Change which section is used as sectionC

**Example**:
```
Instruction: 0x5202  // Set sectionC = section 2
Action:
  sectionC = sections[2].baseAddress
```

**Use case**: Multi-section code (e.g., initialization code in separate section)

#### 0x2A - RelocSmSetSectD
**Format**: `[opcode:7][index:9]`

**Purpose**: Change which section is used as sectionD

#### 0x2B - RelocSmByImport
**Format**: `[opcode:7][index:9]`

**Purpose**: Add import symbol address (small index < 512)

**Example**:
```
Instruction: 0x5605  // 0x2B << 9 | 5
  importIndex = 5
Action:
  memory[relocAddress] = importSymbols[5].tvectAddress
  relocAddress += 4
  importIndex = 6
```

**Use case**: Patching individual import references (not consecutive runs)

### Large Operand Instructions (0x48-0x5F)

These instructions use **TWO 16-bit words** to encode larger values (up to 25 bits).

#### 0x48 - RelocSetPosition (2 words)
**Format**:
- Word 1: `[opcode:7][offset_high:9]` (bits 24-16 of offset)
- Word 2: `[offset_low:16]` (bits 15-0 of offset)

**Purpose**: Set relocAddress to absolute offset within section

**Example**:
```
Word 1: 0x9000  // 0x48 << 9 | 0
Word 2: 0x0ABC
Action:
  relocAddress = (0 << 16) | 0x0ABC = 0x000ABC
```

**Use case**: Jump to specific location when relocations aren't sequential

#### 0x50 - RelocIncrPosition (2 words)
**Format**: Same as SetPosition

**Purpose**: **Increment** relocAddress by specified offset

**Example**:
```
Word 1: 0xA000  // 0x50 << 9 | 0
Word 2: 0x0100
Action:
  relocAddress += 0x0100
```

**Use case**: Skip over large blocks without relocations

#### 0x52 - RelocLgByImport (2 words)
**Format**:
- Word 1: `[opcode:7][index_high:9]`
- Word 2: `[index_low:16]`

**Purpose**: Add import symbol address (large index >= 512)

**Example**:
```
Word 1: 0xA400  // 0x52 << 9 | 0
Word 2: 0x0205
  index = (0 << 16) | 0x0205 = 517
Action:
  memory[relocAddress] = importSymbols[517].tvectAddress
  relocAddress += 4
  importIndex = 518
```

#### 0x58 - RelocLgRepeat (2 words)
**Format**: Same as RelocSetPosition

**Purpose**: Repeat last operation N times (N can be > 512)

#### 0x59 - RelocLgSetOrBySection (2 words)
**Format**:
- Word 1: `[opcode:7][subopcode:9]`
- Word 2: `[value:16]`

**Subopcode table**:
- 0: Set relocAddress
- 1: Set sectionC
- 2: Set sectionD
- 3: Set importIndex
- 4: Relocate with sectionC
- 5: Relocate with sectionD

**Example** (subopcode 4 - BySectC with large runLength):
```
Word 1: 0xB204  // 0x59 << 9 | 4
Word 2: 0x0800  // runLength = 2048
Action:
  for i in 0..2048:
    memory[relocAddress] += sectionC
    relocAddress += 4
```

## Object Files vs Executables

### In Object Files (.o)

Object files contain **raw relocation markers** that describe:
- Which memory locations need patching
- What symbols they reference (by local index)
- Whether they're code/data/import references

**Example relocations** you might see:
- `SetPosition(0x1C)` + `LgByImport(0)` - "At offset 0x1C, reference import #0"
- `BySectC(1)` - "Current location references code section"

**Key point**: Import indices are **local** to each object file

### In Executables (.pef)

The linker converts object relocations to **final CFM bytecode**:
- Assigns **global import indices** across all libraries
- Generates import address table in data section
- Creates **ImportRun** instructions to patch import slots
- Uses **BySectC/BySectD** for resolved internal references

**Example** (linking 3 object files with total 5 imports):
```
Data section layout:
  Offset 0-19:  Import address table (5 imports × 4 bytes)
  Offset 20-31: Main TVect (12 bytes)

Final relocations:
  ImportRun(count=5)  // Patch imports 0-4 at offsets 0-19
  TVector8            // Patch TVect at offset 20-27
```

## How the Linker Uses Relocations

### 1. Reading Object File Relocations

Each section in a PEF object has:
- `relocHeader.relocCount`: Number of relocation instructions
- `relocHeader.firstRelocOffset`: Offset to first instruction

```cpp
// Read relocation instructions for section
uint32_t relocOffset = relocHeader.firstRelocOffset;
uint32_t relocCount = relocHeader.relocCount;
ArrayRef<uint16_t> relocInstructions = readInstructions(relocOffset, relocCount);
```

### 2. Processing Relocations

For **internal references** (symbol defined in another .o file):
- Decode the relocation to find the patch location
- Calculate actual symbol address
- Patch the code/data directly

For **external references** (symbol from shared library):
- Assign global import index
- Create import slot in data section
- Generate `LgByImport` or `ImportRun` instruction for final executable

### 3. Generating Final Relocations

The final executable has one loader section containing:
- Import symbol names table
- Relocation instructions for **runtime patching by CFM**

## Common Patterns

### Pattern 1: Simple Function Call (Internal)

**Object file**:
```
Code:  bl <placeholder>  // Branch with 0 offset
Reloc: SetPosition(0x1C) + BySectC(1)
```

**Linker action**:
1. Find symbol address (e.g., 0x2000 in code section)
2. Calculate branch offset: target - source
3. Patch branch instruction directly
4. **No relocation in final executable** (fully resolved)

### Pattern 2: Imported Function Call

**Object file**:
```
Code:  bl <placeholder>
Reloc: SetPosition(0x1C) + LgByImport(0) // Import #0 = "SysBeep"
```

**Linker action**:
1. Assign global import index (e.g., index 2 after combining all imports)
2. Generate import stub at code section end
3. Patch branch to call stub instead
4. Create import slot at data section offset 8
5. **Generate relocation**: `ImportRun(count=1)` to patch slot at runtime

**Final executable** (CFM loads it):
```
Data[8] = 0x00000000  // Import slot (unpatch)
CFM executes: ImportRun at offset 8
CFM patches:  Data[8] = <SysBeep TVect address from InterfaceLib>
```

### Pattern 3: CodeWarrior-Style Executable

**Data section layout**:
```
Offset 0-11:   Import slots (3 imports × 4 bytes)
Offset 12-23:  Main TVect (code=0x1000, TOC=0x10000, reserved=0)
```

**Final relocations**:
```
ImportRun(count=3)  // Patches offsets 0, 4, 8
TVector8            // Patches offsets 12-19 (adds sectionC/D to 0x1000/0x10000)
```

**After CFM processing**:
```
Data[0]  = <Import0 TVect>
Data[4]  = <Import1 TVect>
Data[8]  = <Import2 TVect>
Data[12] = 0x1000 + codeBase     // Actual code entry point
Data[16] = 0x10000 + dataBase    // Actual TOC pointer
Data[20] = 0                     // Reserved
```

## Implementation Notes

### Decoding Algorithm

```cpp
void processRelocations(ArrayRef<uint16_t> instructions, uint8_t* sectionData) {
    uint32_t relocAddress = 0;
    uint32_t importIndex = 0;
    uint32_t sectionC = codeSectionBase;
    uint32_t sectionD = dataSectionBase;

    for (size_t i = 0; i < instructions.size(); ++i) {
        uint16_t instr = instructions[i];
        uint8_t opcode = (instr >> 9) & 0x7F;
        uint16_t operand = instr & 0x1FF;

        switch (opcode) {
            case 0x20: // BySectC
                uint32_t runLength = operand + 1;
                for (uint32_t j = 0; j < runLength; ++j) {
                    patchWord(sectionData, relocAddress, sectionC);
                    relocAddress += 4;
                }
                break;

            case 0x48: // SetPosition (2 instructions)
                uint16_t offsetHigh = operand;
                uint16_t offsetLow = instructions[++i];
                relocAddress = (offsetHigh << 16) | offsetLow;
                break;

            case 0x52: // LgByImport (2 instructions)
                uint32_t indexHigh = operand;
                uint32_t indexLow = instructions[++i];
                uint32_t index = (indexHigh << 16) | indexLow;

                Symbol* sym = getImportSymbol(index);
                if (sym->isDefined()) {
                    // Internal - patch directly
                    uint32_t targetAddr = sym->getAddress();
                    patchBranch(sectionData, relocAddress, targetAddr);
                } else {
                    // External - create import stub
                    generateImportStub(sym, importIndex++);
                }
                relocAddress += 4;
                break;
        }
    }
}
```

### Helper Functions

```cpp
void patchWord(uint8_t* data, uint32_t offset, uint32_t value) {
    // Add value to existing 32-bit big-endian word
    uint32_t existing = read32be(data + offset);
    uint32_t result = existing + value;
    write32be(data + offset, result);
}

void patchBranch(uint8_t* data, uint32_t offset, uint32_t target) {
    // PowerPC bl instruction: [opcode:6][offset:24][AA:1][LK:1]
    uint32_t source = sectionBase + offset;
    int32_t branchOffset = target - source;
    uint32_t instruction = (18 << 26) | (branchOffset & 0x03FFFFFC) | 1;
    write32be(data + offset, instruction);
}
```

## Debugging Tips

### Enable Verbose Relocation Logging

```bash
ld.lld -flavor pef input.o -o output.pef --verbose
```

Look for:
- "Processing relocations for section .text (N instructions)"
- "Instr[0] = 0xXXXX opcode=YY operand=ZZZ"
- "Patched internal call to 'main' at offset 0xABC"

### Disassemble to Verify Patches

```bash
llvm-objdump -d output.pef
```

Check that:
- Branch instructions have non-zero offsets
- `bl 0x1c` becomes `bl 0x234` (actual function address)
- No self-referencing branches

### Common Issues

**Issue**: "Skipping relocation opcode XX"
- **Cause**: Relocation decoder doesn't handle opcode XX
- **Fix**: Add case to switch statement in processRelocations()

**Issue**: Branch instructions still have zero offset
- **Cause**: Relocations not being applied (wrong section?)
- **Fix**: Verify relocInstructions are loaded from correct section

**Issue**: "Invalid import index"
- **Cause**: LgByImport references symbol not in import table
- **Fix**: Ensure all undefined symbols are added to importIndexMap

## Summary

PEF relocations are a **bytecode language** for describing memory transformations:
- Opcodes 0x00-0x1F: Basic section-relative relocations
- Opcodes 0x20-0x2F: Section, import, and position control
- Opcodes 0x48-0x5F: Large operand variants

The linker's job:
1. **Read** object file relocations
2. **Resolve** internal symbols (patch directly with BySectC/D)
3. **Generate** import infrastructure for external symbols
4. **Emit** final relocations for CFM runtime patching

The key insight: **Object relocations ≠ Final relocations**. The linker transforms local markers into global CFM bytecode.
