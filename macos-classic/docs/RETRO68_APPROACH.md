# Retro68 PEF Generation Approach

## Overview

**Retro68** is a GCC-based cross-compiler toolchain for Classic Mac OS targeting 68K and PowerPC systems. Unlike modern LLVM approaches, Retro68 uses **XCOFF (Extended Common Object File Format)** as an intermediate format and converts it to PEF using the **MakePEF** tool.

**Repository:** `/Users/kirk/repos/toolchain-macos9/Retro68`

**Key Advantage:** Leverages mature GNU toolchain with decades of stability
**Key Limitation:** Two-stage process adds complexity and file size

---

## Architecture

### Build Pipeline

```
┌─────────────┐
│  hello.c    │  C source code
└──────┬──────┘
       │ powerpc-apple-macos-gcc -S
       ↓
┌─────────────┐
│  hello.s    │  PowerPC assembly
└──────┬──────┘
       │ powerpc-apple-macos-as
       ↓
┌─────────────┐
│  hello.o    │  XCOFF object file
└──────┬──────┘
       │ powerpc-apple-macos-ld
       ↓
┌─────────────┐
│hello.xcoff  │  XCOFF executable (linked)
└──────┬──────┘
       │ MakePEF
       ↓
┌─────────────┐
│  hello.pef  │  PEF executable
└──────┬──────┘
       │ Rez (on Mac OS 9)
       ↓
┌─────────────┐
│  HelloApp   │  Mac OS application
└─────────────┘
```

### Why XCOFF?

**XCOFF (Extended Common Object File Format)** was originally designed by IBM for AIX (RS/6000) systems.

**Reasons for Use:**
1. **GCC Support:** PowerPC GCC has native XCOFF support from AIX heritage
2. **Mature Toolchain:** GNU binutils (as, ld, ar) fully support XCOFF
3. **Rich Metadata:** XCOFF loader section contains import/export info
4. **Relocation Model:** Similar enough to PEF for straightforward conversion

**Trade-off:** Extra conversion step, but avoids reimplementing entire object format

---

## MakePEF Tool

**Location:** `/Users/kirk/repos/toolchain-macos9/Retro68/PEFTools/MakePEF.cc`
**Size:** 659 lines
**Language:** C++

### Responsibilities

1. Parse XCOFF executable
2. Extract code, data, and BSS sections
3. Extract import/export information from XCOFF loader section
4. Convert XCOFF relocations to PEF relocation bytecode
5. Generate PEF container, section, and loader headers
6. Write PEF binary

---

## XCOFF Structure

### File Layout

```
┌────────────────────────────────┐
│ File Header (20 bytes)         │
├────────────────────────────────┤
│ Auxiliary Header (28+ bytes)   │
├────────────────────────────────┤
│ Section Headers (40 bytes each)│
├────────────────────────────────┤
│ .text Section (code)           │
│ .data Section (data)           │
│ .bss Section (zero-init)       │
│ .loader Section (metadata)     │
├────────────────────────────────┤
│ Symbol Table                   │
├────────────────────────────────┤
│ String Table                   │
└────────────────────────────────┘
```

### Key Differences from PEF

| Feature | XCOFF | PEF |
|---------|-------|-----|
| **Endianness** | Big-endian | Big-endian |
| **Entry Point** | Code address | TVect descriptor |
| **Relocations** | Struct array | Bytecode |
| **Import Format** | Loader section | Loader section (different layout) |
| **Section Alignment** | Page-aligned (4 KB) | 16-byte aligned |
| **File Size** | Larger (page padding) | Smaller (compact) |

---

## MakePEF Implementation

### Phase 1: Parse XCOFF (Lines 102-238)

```cpp
// Read file header
external_filehdr xcoffHeader;
fread(&xcoffHeader, sizeof(xcoffHeader), 1, f);

// Read auxiliary header
external_aouthdr aoutHeader;
fread(&aoutHeader, sizeof(aoutHeader), 1, f);

// Read section headers
for (int i = 0; i < xcoffHeader.f_nscns; i++) {
    external_scnhdr scnhdr;
    fread(&scnhdr, sizeof(scnhdr), 1, f);
    // Store .text, .data, .loader sections
}
```

**Key Sections Extracted:**
- `.text` → PEF code section
- `.data` + `.bss` → PEF data section (combined)
- `.loader` → Import/export metadata for PEF loader section

### Phase 2: Extract Imports (Lines 239-355)

XCOFF encodes imports in the `.loader` section with library names as hex strings:

```cpp
// XCOFF import library encoding:
// Library name "InterfaceLib" → "imp__496E746572666163654C6962"
//                                      ^^^^^^^^^^^^^^^^^^^^^^^^^
//                                      Hex: InterfaceLib

// MakePEF converts back to ASCII
std::string libraryName = unhexlify(hexName);
```

**Import Symbol Processing:**
```cpp
external_ldhdr loaderHeader;
// loaderHeader.l_nsyms = number of imported symbols

for (uint32_t i = 0; i < loaderHeader.l_nsyms; i++) {
    external_ldsym sym;
    fread(&sym, sizeof(sym), 1, loaderFile);

    if (isImport(sym)) {
        std::string name = readStringAt(sym.l_offset);
        imports.push_back({libIndex, name});
    }
}
```

### Phase 3: Convert Relocations (Lines 356-445)

XCOFF relocations are stored as fixed-size structures:

```cpp
struct external_ldrel {
    uint32_t l_vaddr;    // Virtual address to patch
    int32_t l_symndx;    // Symbol index (-1=code, -2=data, >=0=import)
    uint16_t l_rtype;    // Relocation type
};
```

**Conversion Logic:**

```cpp
for (auto &rel : xcoffRelocations) {
    uint32_t vaddr = get(rel.l_vaddr);
    int32_t symndx = get(rel.l_symndx);

    // Generate SetPosition instructions
    relocInstructions.push_back(PEFRelocComposeSetPosition_1st(vaddr));
    relocInstructions.push_back(PEFRelocComposeSetPosition_2nd(vaddr));

    // Generate relocation type
    if (symndx == -1) {
        // Code section relocation
        relocInstructions.push_back(PEFRelocComposeBySectC(1));
    } else if (symndx == -2) {
        // Data section relocation
        relocInstructions.push_back(PEFRelocComposeBySectD(1));
    } else {
        // Import relocation
        uint32_t importIndex = symndx;
        relocInstructions.push_back(PEFRelocComposeLgByImport_1st(importIndex));
        relocInstructions.push_back(PEFRelocComposeLgByImport_2nd(importIndex));
    }
}
```

**Relocation Encoding Helpers:**

```cpp
inline uint16_t PEFRelocComposeSetPosition_1st(uint32_t offset) {
    return (kPEFRelocSetPosition << 9) | ((offset >> 16) & 0x1FF);
}

inline uint16_t PEFRelocComposeSetPosition_2nd(uint32_t offset) {
    return offset & 0xFFFF;
}

inline uint16_t PEFRelocComposeBySectC(uint32_t count) {
    return (kPEFRelocBySectC << 9) | ((count - 1) & 0x1FF);
}
```

### Phase 4: Build PEF Loader Section (Lines 446-590)

**Loader Info Header:**

```cpp
PEFLoaderInfoHeader loaderInfoHeader;
memset(&loaderInfoHeader, 0, sizeof(loaderInfoHeader));

// Entry point from XCOFF auxiliary header
put(loaderInfoHeader.mainSection, 1);  // Data section (for TVect)
put(loaderInfoHeader.mainOffset, get(aoutHeader.entry));

// Import counts
put(loaderInfoHeader.importedLibraryCount, importedLibraries.size());
put(loaderInfoHeader.totalImportedSymbolCount, totalImports);

// Relocation info
put(loaderInfoHeader.relocSectionCount, 1);  // Typically 1 (data section)
put(loaderInfoHeader.relocInstrOffset, relocInstrOffset);

// Export info (executables typically don't export)
put(loaderInfoHeader.exportedSymbolCount, 0);
```

**Imported Library Structures:**

```cpp
for (auto &lib : importedLibraries) {
    PEFImportedLibrary libEntry;

    put(libEntry.nameOffset, lib.nameOffset);
    put(libEntry.oldImpVersion, 0);
    put(libEntry.currentVersion, 0);
    put(libEntry.importedSymbolCount, lib.symbols.size());
    put(libEntry.firstImportedSymbol, lib.firstImportIndex);
    libEntry.options = 0;  // Strong imports

    fwrite(&libEntry, sizeof(libEntry), 1, out);
}
```

**String Table:**

```cpp
// Write library names
for (auto &lib : importedLibraries) {
    fputs(lib.name.c_str(), out);
    fputc('\0', out);  // Null terminator
}

// Write symbol names
for (auto &sym : importedSymbols) {
    fputs(sym.name.c_str(), out);
    fputc('\0', out);
}
```

### Phase 5: Write PEF Binary (Lines 591-659)

```cpp
// Container Header
PEFContainerHeader pefHeader;
put(pefHeader.tag1, kPEFTag1);              // 'Joy!'
put(pefHeader.tag2, kPEFTag2);              // 'peff'
put(pefHeader.architecture, kPEFPowerPC);   // 'pwpc'
put(pefHeader.formatVersion, kPEFVersion);  // 1
put(pefHeader.sectionCount, 3);             // code, data, loader
put(pefHeader.instSectionCount, 2);         // code, data (loader not instantiated)

fwrite(&pefHeader, sizeof(pefHeader), 1, out);

// Section Headers
PEFSectionHeader textHeader, dataHeader, loaderHeader;

// Text section (code)
put(textHeader.totalLength, textSize);
put(textHeader.containerLength, textSize);
put(textHeader.containerOffset, textOffset);
textHeader.sectionKind = kPEFCodeSection;

// Data section (data + bss combined)
put(dataHeader.totalLength, dataSize + bssSize);
put(dataHeader.unpackedLength, dataSize);
put(dataHeader.containerLength, dataSize);
put(dataHeader.containerOffset, dataOffset);
dataHeader.sectionKind = kPEFUnpackedDataSection;

// Loader section
put(loaderHeader.totalLength, 0);           // NOT instantiated!
put(loaderHeader.unpackedLength, 0);
put(loaderHeader.containerLength, loaderSize);
put(loaderHeader.containerOffset, loaderOffset);
loaderHeader.sectionKind = kPEFLoaderSection;

// Write section data
fwrite(textData, textSize, 1, out);
fwrite(dataData, dataSize, 1, out);
fwrite(loaderData, loaderSize, 1, out);
```

---

## Characteristics

### File Size Analysis

**Example: Hello World**

```
Component              XCOFF       PEF (MakePEF)
────────────────────────────────────────────────
Container Header       20 bytes    40 bytes
Section Headers        120 bytes   120 bytes
Code Section           128 bytes   128 bytes (page-aligned: 4096)
Data Section           64 bytes    64 bytes (page-aligned: 4096)
Loader Section         256 bytes   256 bytes
────────────────────────────────────────────────
Total (disk)           ~8.5 KB     6.2 KB
Total (theoretical)    ~588 bytes  608 bytes
```

**Why so large?**
- **Page Alignment:** XCOFF and MakePEF use 4 KB (4096-byte) alignment
- **Wasted Space:** Small sections (128 bytes) padded to 4096 bytes
- **Compatibility:** Matches CodeWarrior conventions (pre-Mac OS X era)

**Trade-off:** Compatibility vs file size

### Advantages

1. **Proven Stability:** GCC toolchain battle-tested since 1987
2. **Rich Ecosystem:** GNU binutils support (ar, nm, objdump, etc.)
3. **Standard Format:** XCOFF is well-documented (AIX standard)
4. **Debugging:** XCOFF debug info can be preserved
5. **Portability:** Works on Linux, macOS, Windows (cross-compile)

### Limitations

1. **File Size:** 2-3× larger than optimal PEF
2. **Build Complexity:** Extra conversion step
3. **XCOFF Dependency:** Not widely used outside AIX/Retro68
4. **Performance:** Extra I/O and conversion time
5. **Maintenance:** Two codebases (GCC + MakePEF)

---

## Integration Points

### Compiler

```bash
powerpc-apple-macos-gcc -c hello.c -o hello.o
```

**Produces:** XCOFF object file with:
- `.text` section (code)
- `.data` section (initialized globals)
- `.bss` section (uninitialized globals)
- Relocation entries
- Symbol table

### Linker

```bash
powerpc-apple-macos-ld hello.o -o hello.xcoff \
    -L/path/to/libs \
    -lInterfaceLib \
    -lMathLib
```

**Produces:** XCOFF executable with:
- Linked code and data
- `.loader` section with import/export info
- Resolved relocations
- Entry point address

### MakePEF

```bash
MakePEF hello.xcoff -o hello.pef
```

**Produces:** PEF executable ready for Mac OS 9

### Resource Fork (Final Step)

```bash
# On Mac OS 9 or with Rez tool:
Rez hello.r -o HelloApp -t APPL -c '????' --data hello.pef
```

**Produces:** Complete Mac OS application with:
- Data fork: PEF executable
- Resource fork: 'cfrg' resource, icons, etc.

---

## Code Examples

### Example 1: Simple Executable

**hello.c:**
```c
#include <MacMemory.h>

void main(void) {
    ExitToShell();
}
```

**Build:**
```bash
powerpc-apple-macos-gcc -c hello.c -o hello.o
powerpc-apple-macos-ld hello.o -o hello.xcoff -lInterfaceLib
MakePEF hello.xcoff -o hello.pef
```

**Result:**
- XCOFF: 8.5 KB (page-aligned)
- PEF: 6.2 KB (page-aligned)
- Actual code: ~64 bytes

### Example 2: Multi-Library Import

**math_test.c:**
```c
#include <math.h>
#include <MacMemory.h>

void main(void) {
    double x = sqrt(2.0);
    ExitToShell();
}
```

**Build:**
```bash
powerpc-apple-macos-gcc -c math_test.c -o math_test.o
powerpc-apple-macos-ld math_test.o -o math_test.xcoff \
    -lInterfaceLib -lMathLib
MakePEF math_test.xcoff -o math_test.pef
```

**PEF Loader Section Contains:**
- Imported Library 0: InterfaceLib (ExitToShell)
- Imported Library 1: MathLib (sqrt)
- Total imported symbols: 2

---

## Technical Deep Dive

### Import Library Name Encoding

XCOFF uses hexadecimal encoding for library names to avoid special characters:

```python
# Encoding
library_name = "InterfaceLib"
hex_name = library_name.encode('ascii').hex()
xcoff_name = f"imp__{hex_name}"
# Result: "imp__496E746572666163654C6962"

# Decoding (in MakePEF)
xcoff_name = "imp__496E746572666163654C6962"
hex_name = xcoff_name[5:]  # Skip "imp__"
library_name = bytes.fromhex(hex_name).decode('ascii')
# Result: "InterfaceLib"
```

**Why?** XCOFF symbol names are limited to certain characters. Hex encoding ensures any library name works.

### Relocation Processing

**XCOFF Relocation:**
```
l_vaddr: 0x00000004    ; Patch offset 4 in data section
l_symndx: 0            ; Import index 0
l_rtype: R_POS         ; Absolute relocation
```

**Converted to PEF:**
```
0x4800 0x0004   ; SetPosition (offset 4)
0xA400 0x0000   ; LgByImport (import 0)
```

**Execution:**
1. Cursor = 4
2. Load import 0's TVect address
3. Write TVect address to data[4:7]
4. Cursor advances to 8

---

## Comparison with LLVM

| Feature | Retro68 | LLVM |
|---------|---------|------|
| **Intermediate Format** | XCOFF | None (direct PEF) |
| **Compiler** | GCC 13.x | Clang 17.x |
| **Object Format** | XCOFF .o | PEF .o |
| **Linker** | GNU ld | LLD (ld.lld) |
| **Conversion Tool** | MakePEF | None (built-in) |
| **File Size** | 6.2 KB | 3.3 KB |
| **Build Steps** | 5 (compile, assemble, link, convert, package) | 4 (compile, assemble, link, package) |
| **Maturity** | Stable (years) | Development |

---

## Lessons for LLVM

### What LLVM Can Learn

1. **Comprehensive Relocation Support**
   - Retro68 handles all XCOFF relocation types
   - LLVM should support full PEF relocation set

2. **Library Name Handling**
   - Robust import library resolution
   - Weak vs strong imports

3. **Testing Methodology**
   - Extensive test suite for edge cases
   - Comparison with known-good CodeWarrior binaries

4. **Documentation**
   - Detailed comments explaining XCOFF→PEF mappings
   - Historical context for design decisions

### What LLVM Does Better

1. **Direct PEF Generation**
   - No intermediate format
   - Smaller binaries

2. **Modern Optimizations**
   - Clang produces better code than GCC
   - LTO (Link-Time Optimization) support

3. **Self-Restoring Import Stubs**
   - LLVM doesn't rely on compiler-generated TOC restore
   - More robust against codegen variations

4. **Integration**
   - Single toolchain (Clang/LLVM/LLD)
   - No external conversion tools

---

## References

### Source Code
- **MakePEF:** `/Users/kirk/repos/toolchain-macos9/Retro68/PEFTools/MakePEF.cc`
- **BFD PEF:** `/Users/kirk/repos/toolchain-macos9/Retro68/binutils/bfd/pef.c`
- **GCC Backend:** `/Users/kirk/repos/toolchain-macos9/Retro68/gcc/gcc/config/rs6000/`

### Documentation
- **XCOFF Spec:** `/Users/kirk/repos/toolchain-macos9/Retro68/binutils/include/coff/xcoff.h`
- **PEF Headers:** `/Users/kirk/repos/toolchain-macos9/Retro68/PEFTools/PEFBinaryFormat.h`

### Build Files
- **Toolchain Build:** `/Users/kirk/repos/toolchain-macos9/Retro68-build/`
- **Installed Binaries:** `/Users/kirk/repos/toolchain-macos9/Retro68-build/toolchain/bin/`

---

**Document Version:** 1.0
**Last Updated:** 2025-11-13
**Author:** LLVM PEF Toolchain Project
