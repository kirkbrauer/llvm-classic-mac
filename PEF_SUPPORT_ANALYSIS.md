# PEF Support Analysis for LLVM

## Executive Summary

This document analyzes how to add full PEF (Preferred Executable Format) support to LLVM for Classic Mac OS development. The recommendation is to build an external `elf2pef` tool that converts from ELF (LLVM's native PowerPC output) to PEF.

**Key Findings:**
- No existing PEF support in LLVM codebase
- LLVM has well-established architecture for adding binary formats
- PowerPC backend exists but only supports ELF on Linux (no Darwin/Classic Mac OS)
- PEF format is well-documented with clear structure

---

## Table of Contents

1. [Current State of LLVM](#current-state-of-llvm)
2. [PEF Format Specification](#pef-format-specification)
3. [Proposed Architecture](#proposed-architecture)
4. [Implementation Approaches](#implementation-approaches)
5. [Recommendations](#recommendations)

---

## Current State of LLVM

### PEF/Classic Mac Support: None Found

**Search Results:**
- No files containing "PEF", "CFM" (Code Fragment Manager), or "Classic Mac" references
- No existing Classic Mac OS toolchain support
- Mach-O support exists but only for modern macOS (Intel/ARM), not Classic Mac PowerPC

**PowerPC Backend:**
- Located: `llvm/lib/Target/PowerPC/`
- Only supports ELF format for Linux (PPC64LinuxTargetObjectFile)
- File: `llvm/lib/Target/PowerPC/PPCTargetObjectFile.cpp:17-20`
- No Darwin or Classic Mac specific handling

### LLVM Binary Format Architecture

LLVM has an excellent, extensible architecture for supporting multiple binary formats:

**Core Components:**

1. **Binary Type Enum** (`llvm/include/llvm/Object/Binary.h:41-76`)
   - Type IDs: `ID_COFF`, `ID_ELF32L/B`, `ID_ELF64L/B`, `ID_MachO32L/B`, `ID_MachO64L/B`, `ID_XCOFF32/64`, `ID_Wasm`, `ID_GOFF`, etc.
   - Factory pattern in `createBinary()` autodetects format via magic bytes

2. **ObjectFile Interface** (`llvm/include/llvm/Object/ObjectFile.h`)
   - Abstract interface with methods for:
     - Sections (iterators, names, contents, attributes)
     - Symbols (iterators, names, addresses, types)
     - Relocations (iterators, types, offsets)
   - Format-specific implementations: `ELFObjectFile`, `MachOObjectFile`, `COFFObjectFile`, `WasmObjectFile`, etc.

3. **Magic Number Detection** (`llvm/include/llvm/BinaryFormat/Magic.h`)
   - `file_magic` enum with all supported formats
   - `identify_magic()` function to detect format from first N bytes

4. **Factory Pattern** (`llvm/lib/Object/Binary.cpp:45-104`)
   - `createBinary()` uses magic detection to instantiate correct object file type
   - Switch statement routes to format-specific parsers

**Supported Formats:**
- ELF (Executable and Linkable Format)
- Mach-O (Macintosh Object - modern macOS)
- COFF (Common Object File Format - Windows)
- XCOFF (Extended COFF - IBM AIX)
- WebAssembly (Wasm)
- GOFF (IBM z/OS)
- Archives (ar format)

### Binary Format Tools in LLVM

**Tool Directory:** `llvm/tools/`

**Key Converter Tools:**

1. **obj2yaml** - Converts object files to YAML representation
   - Pattern: `<format>2yaml.cpp` (e.g., `elf2yaml.cpp`, `macho2yaml.cpp`, `wasm2yaml.cpp`)
   - Main driver: `llvm/tools/obj2yaml/obj2yaml.cpp`
   - Uses format detection and delegates to format-specific converters

2. **yaml2obj** - Converts YAML back to object files
   - Implements round-trip binary format conversion
   - Each format has YAML schema: `llvm/include/llvm/ObjectYAML/<Format>YAML.h`

3. **llvm-objdump** - Disassemble and display object files
4. **llvm-readobj** - Display raw object file information
5. **llvm-objcopy** - Copy and transform object files

**Example Pattern (Wasm):**
```
llvm/include/llvm/BinaryFormat/Wasm.h         - Format constants & structures
llvm/include/llvm/Object/Wasm.h               - WasmObjectFile class
llvm/lib/Object/WasmObjectFile.cpp            - Parser implementation
llvm/include/llvm/ObjectYAML/WasmYAML.h       - YAML schema
llvm/lib/ObjectYAML/WasmYAML.cpp              - YAML serialization
llvm/tools/obj2yaml/wasm2yaml.cpp             - Converter tool
```

---

## PEF Format Specification

### Overview

**Preferred Executable Format (PEF)** was developed by Apple Computer for Classic Mac OS:
- Also known as: Code Fragment Manager (CFM) files
- Optimized for: RISC processors (PowerPC)
- Used in: Mac OS 7 through Mac OS 9
- Replaced by: Mach-O in Mac OS X
- Documentation: "Mac OS Runtime Architectures" (Apple, 1997)

### Container Header Structure

**Size:** 40 bytes
**Fields:**

| Offset | Size | Field | Value/Description |
|--------|------|-------|-------------------|
| 0x00 | 4 | Tag1 | "Joy!" (magic number) |
| 0x04 | 4 | Tag2 | "peff" (magic number) |
| 0x08 | 4 | Architecture | "pwpc" (PowerPC) or "m68k" (Motorola 68K) |
| 0x0C | 4 | FormatVersion | PEF format version |
| 0x10 | 4 | DateTimeStamp | Seconds from January 1, 1904 |
| 0x14 | 4 | OldDefVersion | Old definition version |
| 0x18 | 4 | OldImpVersion | Old implementation version |
| 0x1C | 4 | CurrentVersion | Current version |
| 0x20 | 2 | SectionCount | Total sections in container |
| 0x22 | 2 | InstSectionCount | Instantiated sections |
| 0x24 | 4 | Reserved | Must be 0 |

**Byte Order:** Big-endian
**Alignment:** 16 bytes for file-mapped containers

### Section Header Structure

**Size:** 28 bytes per section
**Fields:**

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x00 | 4 | NameOffset | Offset to section name (-1 if none) |
| 0x04 | 4 | DefaultAddress | Preferred load address |
| 0x08 | 4 | TotalSize | Memory required at runtime |
| 0x0C | 4 | UnpackedSize | Initialized data size |
| 0x10 | 4 | PackedSize | Container data size |
| 0x14 | 4 | ContainerOffset | Offset from container start |
| 0x18 | 1 | SectionKind | Section type (see below) |
| 0x19 | 1 | ShareKind | Sharing mode |
| 0x1A | 1 | Alignment | Alignment as power of 2 |
| 0x1B | 1 | Reserved | Must be 0 |

### Section Types (SectionKind)

| Value | Name | Description |
|-------|------|-------------|
| 0 | kindCode | Read-only executable code |
| 1 | kindUnpackedData | Read/write uninitialized data |
| 2 | kindPatternInitializedData | Read/write pattern-initialized data |
| 3 | kindConstant | Read-only data |
| 4 | kindLoader | Imports/exports/entry points/relocations |
| 5 | kindDebug | Reserved for debugging |
| 6 | kindExecutableData | Read/write executable code |
| 7 | kindException | Reserved for exception handling |
| 8 | kindTraceback | Reserved for traceback tables |

### Loader Section Structure

**Size:** 56-byte header + variable data
**Loader Header Fields:**

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x00 | 4 | MainSection | Section index containing main |
| 0x04 | 4 | MainOffset | Offset to main entry point |
| 0x08 | 4 | InitSection | Section index for initialization |
| 0x0C | 4 | InitOffset | Offset to init function |
| 0x10 | 4 | TermSection | Section index for termination |
| 0x14 | 4 | TermOffset | Offset to term function |
| 0x18 | 4 | ImportedLibraryCount | Number of imported libraries |
| 0x1C | 4 | TotalImportedSymbolCount | Total imported symbols |
| 0x20 | 4 | RelocSectionCount | Sections with relocations |
| 0x24 | 4 | RelocInstrOffset | Offset to relocation instructions |
| 0x28 | 4 | LoaderStringsOffset | Offset to string table |
| 0x2C | 4 | ExportHashOffset | Offset to export hash table |
| 0x30 | 4 | ExportHashTablePower | Hash table size (2^n) |
| 0x34 | 4 | ExportedSymbolCount | Number of exported symbols |

**Loader Section Components (in order):**
1. Loader header (56 bytes)
2. Imported library table
3. Imported symbol table
4. Relocation headers table
5. Relocation instructions
6. Loader string table
7. Export hash table
8. Export key table
9. Exported symbol table

### Key Differences from ELF

| Feature | ELF | PEF |
|---------|-----|-----|
| Magic | 0x7F 'E' 'L' 'F' | "Joy!" "peff" |
| Endianness | Field in header | Always big-endian |
| Sections | Many standard types | 9 defined kinds |
| Relocations | Multiple formats per arch | Compact bytecode instructions |
| Symbols | Standard symbol table | Hash table + key table |
| Entry Point | Single entry in header | Main/Init/Term in loader |
| Imports/Exports | Dynamic section | Loader section |
| Code/Data Separation | Via flags | Via section kind |

---

## Proposed Architecture

### Option 1: External elf2pef Tool (RECOMMENDED)

**Location:** `llvm/tools/elf2pef/`

**Architecture:**
```
┌─────────────────────────────────────────────────────┐
│                  LLVM Compilation                    │
│  Source → LLVM IR → PowerPC ASM → ELF Object        │
└────────────────────┬────────────────────────────────┘
                     │ ELF File
                     ↓
         ┌───────────────────────┐
         │      elf2pef Tool      │
         │  (Standalone Binary)   │
         └───────────┬───────────┘
                     │ PEF File
                     ↓
         ┌───────────────────────┐
         │   Classic Mac OS       │
         │  Code Fragment Mgr     │
         └───────────────────────┘
```

**Components:**

1. **ELF Parser**
   - Use existing `llvm/include/llvm/Object/ELF.h` and `ELFObjectFile` class
   - Read sections, symbols, relocations from PowerPC ELF

2. **PEF Format Library**
   - `llvm/include/llvm/BinaryFormat/PEF.h` - Format constants and structures
   - `llvm/include/llvm/Object/PEF.h` - PEFObjectFile class (read-only for verification)
   - `llvm/lib/Object/PEFObjectFile.cpp` - Parser implementation

3. **PEF Writer**
   - `llvm/lib/Object/PEFWriter.cpp` - PEF file generation
   - Container header generation
   - Section creation and packing
   - Loader section construction
   - Relocation instruction encoding

4. **Conversion Tool**
   - `llvm/tools/elf2pef/elf2pef.cpp` - Main driver
   - Command-line interface
   - Section mapping logic
   - Symbol table conversion
   - Relocation translation

**Advantages:**
- No changes to LLVM backend required
- Works with existing PowerPC ELF toolchain
- Simpler to develop and test
- Can be external project initially
- Easier to debug conversion issues
- Reuses mature ELF generation code

**Disadvantages:**
- Two-step process (compile to ELF, then convert)
- Potential information loss in conversion
- No direct PEF generation from codegen

### Option 2: Native PEF Support in LLVM Backend

**Architecture:**
```
┌─────────────────────────────────────────────────────┐
│              LLVM Compilation Pipeline               │
│  Source → LLVM IR → PowerPC ASM → PEF Object        │
│                                     ↑                 │
│                         PPCPEFTargetObjectFile       │
│                         PPCPEFTargetStreamer         │
└─────────────────────────────────────────────────────┘
```

**Required Changes:**

1. **Binary Format Support** (similar to Option 1)
   - `llvm/include/llvm/BinaryFormat/PEF.h`
   - `llvm/include/llvm/Object/PEF.h`
   - `llvm/lib/Object/PEFObjectFile.cpp`

2. **PowerPC Backend Extensions**
   - New Triple support: `powerpc-apple-macos9` or `powerpc-apple-classicmacos`
   - `PPCPEFTargetObjectFile` class (extends `TargetLoweringObjectFileImpl`)
   - `PPCPEFTargetStreamer` for PEF-specific ASM directives
   - Modify `PPCTargetMachine.cpp` to support PEF object format

3. **MC Layer Extensions**
   - `MCPEFStreamer` class for PEF emission
   - `MCPEFObjectWriter` for writing PEF containers
   - Relocation encoding for PEF format

4. **YAML Round-Trip Support**
   - `llvm/include/llvm/ObjectYAML/PEFYAML.h`
   - `llvm/lib/ObjectYAML/PEFYAML.cpp`
   - `llvm/tools/obj2yaml/pef2yaml.cpp`
   - `llvm/tools/yaml2obj/pef2yaml.cpp`

**Advantages:**
- Single-step compilation to PEF
- No conversion artifacts
- Full integration with LLVM toolchain
- Can optimize specifically for PEF format

**Disadvantages:**
- Significantly more complex
- Requires extensive LLVM backend knowledge
- Harder to maintain
- May need Classic Mac ABI support
- Longer development time

### Option 3: YAML-Based Workflow

**Architecture:**
```
Source → ELF → YAML → Manual Edit → YAML → PEF
         ↑           ↑                      ↑
    llvm-clang   elf2yaml              yaml2pef
```

**Advantages:**
- Human-readable intermediate format
- Easy to debug and inspect
- Flexible for experimentation

**Disadvantages:**
- Not suitable for production builds
- Multiple conversion steps
- Performance overhead

---

## Implementation Approaches

### Recommended Approach: External elf2pef Tool

#### Phase 1: Format Support Libraries (Foundation)

**Goal:** Basic PEF reading/writing infrastructure

**Tasks:**

1. **Create PEF Format Header**
   - File: `llvm/include/llvm/BinaryFormat/PEF.h`
   - Define all PEF constants and structures:
     - Container header struct
     - Section header struct
     - Loader header struct
     - Section kind enum
     - Share kind enum
     - Relocation opcode enum

2. **Create PEF Object File Reader** (for verification/testing)
   - File: `llvm/include/llvm/Object/PEF.h`
   - Class: `PEFObjectFile` extends `ObjectFile`
   - Implement required virtual methods:
     - `section_begin()`, `section_end()`
     - `symbol_begin()`, `symbol_end()`
     - `getSymbolName()`, `getSymbolAddress()`
     - `getSectionName()`, `getSectionContents()`
     - `getBytesInAddress()`, `getFileFormatName()`
     - `getArch()` → `Triple::ppc`

   - File: `llvm/lib/Object/PEFObjectFile.cpp`
   - Implement parsing logic:
     - Parse container header
     - Parse section headers
     - Parse loader section
     - Extract symbols from loader
     - Handle relocations

3. **Register PEF in Binary Factory**
   - Modify: `llvm/include/llvm/Object/Binary.h`
     - Add `ID_PEF` to type enum (lines 41-76)
     - Add `isPEF()` method

   - Modify: `llvm/include/llvm/BinaryFormat/Magic.h`
     - Add `pef_executable` to `file_magic` enum

   - Modify: `llvm/lib/BinaryFormat/Magic.cpp`
     - Add PEF magic detection: check for "Joy!" "peff"

   - Modify: `llvm/lib/Object/Binary.cpp`
     - Add `case file_magic::pef_executable:` in `createBinary()` (line 50)
     - Call `PEFObjectFile::create()`

4. **Add Build System Integration**
   - Modify: `llvm/lib/Object/CMakeLists.txt`
     - Add `PEFObjectFile.cpp`

   - Modify: `llvm/lib/BinaryFormat/CMakeLists.txt`
     - No changes needed (Magic.cpp already included)

**Deliverable:** LLVM can recognize and parse PEF files

#### Phase 2: elf2pef Converter Tool

**Goal:** Working ELF → PEF conversion tool

**Tasks:**

1. **Create Tool Structure**
   - Directory: `llvm/tools/elf2pef/`
   - Files:
     - `elf2pef.cpp` - Main driver
     - `PEFWriter.h` - PEF writer class
     - `PEFWriter.cpp` - PEF generation logic
     - `CMakeLists.txt` - Build configuration

2. **Implement PEF Writer**

   **Class: PEFWriter**
   ```cpp
   class PEFWriter {
     // Input ELF file
     const ELFObjectFile<ELFT> *ELFObj;

     // Output PEF structures
     PEFContainerHeader ContainerHdr;
     std::vector<PEFSectionHeader> SectionHdrs;
     std::vector<uint8_t> SectionData;
     PEFLoaderSection LoaderSection;

   public:
     Error writeToFile(StringRef Path);

   private:
     Error convertSections();
     Error convertSymbols();
     Error convertRelocations();
     Error buildLoaderSection();
   };
   ```

   **Section Mapping Logic:**
   ```
   ELF Section          →  PEF Section Kind
   ────────────────────────────────────────
   .text                →  kindCode
   .rodata              →  kindConstant
   .data                →  kindUnpackedData
   .bss                 →  kindUnpackedData (zero-initialized)
   .init_array          →  Loader initSection
   .fini_array          →  Loader termSection
   (synthetic)          →  kindLoader (created by converter)
   ```

   **Symbol Table Conversion:**
   - Extract global/weak symbols from ELF `.symtab`
   - Build PEF export hash table
   - Create export key table
   - Generate exported symbol table
   - Import symbols from undefined references

   **Relocation Translation:**
   - Map ELF PowerPC relocations to PEF relocation opcodes
   - Common mappings:
     ```
     R_PPC_ADDR32   →  RelocLgSetOrBySection + RelocLgRepeat
     R_PPC_REL24    →  RelocLgSetSectC + branch reloc
     R_PPC_ADDR16_HA →  PEF hi16 reloc sequence
     R_PPC_ADDR16_LO →  PEF lo16 reloc sequence
     ```

3. **Implement Main Driver**

   **Command-Line Interface:**
   ```
   elf2pef [options] <input.o> -o <output.pef>

   Options:
     --arch <arch>          Target architecture (default: pwpc)
     --entry <symbol>       Entry point symbol (default: _main)
     --init <symbol>        Initialization function
     --term <symbol>        Termination function
     --version <version>    PEF version (default: 1)
     --timestamp <time>     Set timestamp (default: current time)
   ```

   **Processing Pipeline:**
   ```cpp
   int main(int argc, char *argv[]) {
     // 1. Parse command line
     cl::ParseCommandLineOptions(argc, argv);

     // 2. Load input ELF file
     Expected<OwningBinary<Binary>> BinaryOrErr =
         createBinary(InputFilename);

     // 3. Cast to ELF object
     auto *ELFObj = dyn_cast<ELFObjectFile>(BinaryOrErr->getBinary());

     // 4. Create PEF writer
     PEFWriter Writer(ELFObj);

     // 5. Perform conversion
     if (Error E = Writer.writeToFile(OutputFilename))
       return reportError(std::move(E));

     return 0;
   }
   ```

4. **Add Testing**
   - Directory: `llvm/test/tools/elf2pef/`
   - Test cases:
     - `basic.test` - Simple hello world conversion
     - `relocations.test` - Various relocation types
     - `symbols.test` - Symbol import/export
     - `sections.test` - Multiple section types
     - `roundtrip.test` - Convert and verify

**Deliverable:** Working elf2pef tool

#### Phase 3: Integration and Testing

**Tasks:**

1. **Build System Integration**
   - Add elf2pef to `llvm/tools/CMakeLists.txt`
   - Add installation rules
   - Add documentation

2. **Test Suite**
   - Create PowerPC test programs
   - Compile to ELF with `clang -target powerpc-linux-gnu`
   - Convert with elf2pef
   - Verify PEF structure (using PEFObjectFile reader)
   - Optionally: test on Classic Mac OS emulator (QEMU, SheepShaver)

3. **Documentation**
   - Usage guide: `llvm/docs/CommandGuide/elf2pef.rst`
   - Format reference: `llvm/docs/PEFFormat.rst`
   - Limitations and known issues

**Deliverable:** Production-ready elf2pef tool

#### Phase 4: Optional Enhancements

1. **YAML Support**
   - `llvm/include/llvm/ObjectYAML/PEFYAML.h`
   - `llvm/tools/obj2yaml/pef2yaml.cpp`
   - `llvm/tools/yaml2obj/pef2yaml.cpp`

2. **llvm-objdump PEF Support**
   - Add PEF disassembly support
   - Display loader section information

3. **llvm-readobj PEF Support**
   - Display PEF headers
   - Show sections, symbols, relocations

---

## Recommendations

### Primary Recommendation: External elf2pef Tool

**Rationale:**
1. **Lower Risk** - No changes to LLVM backend required
2. **Faster Development** - Reuses existing ELF infrastructure
3. **Easier Maintenance** - Standalone tool easier to update
4. **Incremental Approach** - Can start external, move internal later
5. **Proven Pattern** - Similar to other binary format tools

**Development Timeline:**
- Phase 1 (Format Support): 2-3 weeks
- Phase 2 (Converter Tool): 4-6 weeks
- Phase 3 (Testing/Integration): 2-3 weeks
- **Total: 8-12 weeks** for working implementation

### Secondary Recommendation: Native PEF Backend Support

**When to Consider:**
1. Need to generate optimal PEF code directly
2. Want to support Classic Mac ABI fully
3. Have long-term commitment to Classic Mac toolchain
4. Have LLVM backend expertise available

**Development Timeline:**
- Format Support: 2-3 weeks
- Backend Integration: 8-12 weeks
- MC Layer: 6-8 weeks
- Testing/Debug: 4-6 weeks
- **Total: 20-30 weeks** for production quality

### Implementation Path

**Recommended Sequence:**

1. **Start with elf2pef** (External Tool)
   - Quick proof of concept
   - Learn PEF format details
   - Identify conversion challenges
   - Produce working toolchain

2. **Evaluate Results**
   - Test on real Classic Mac OS
   - Measure conversion accuracy
   - Identify limitations

3. **Decision Point**
   - If elf2pef sufficient → productionize and maintain
   - If limitations found → consider native backend
   - If performance critical → implement native support

4. **Optional: Migrate to Native**
   - Use elf2pef experience to guide backend implementation
   - Reuse PEF format libraries
   - Maintain elf2pef for compatibility

### Key Success Factors

1. **Testing Infrastructure**
   - Access to Classic Mac OS environment (emulator)
   - Test suite of representative programs
   - PEF verification tools

2. **Documentation**
   - Thorough PEF format documentation
   - Clear usage examples
   - Known limitations documented

3. **Relocation Handling**
   - Accurate ELF → PEF relocation mapping
   - Test all PowerPC relocation types
   - Handle edge cases (cross-section, large offsets)

4. **Symbol Management**
   - Correct import/export handling
   - Hash table generation
   - Name mangling compatibility

---

## References

1. **Apple Documentation**
   - "Mac OS Runtime Architectures" (1997)
     - Chapter 8: PEF Structure
     - Available at: developer.apple.com/library/archive/

2. **Open Source Implementations**
   - github.com/decomp/exp/blob/master/bin/pef/pef.go
     - Go language PEF parser
     - Good reference for structure definitions

3. **LLVM Architecture**
   - llvm/include/llvm/Object/Binary.h
   - llvm/include/llvm/Object/ObjectFile.h
   - llvm/lib/Object/Binary.cpp

4. **Similar Tools**
   - llvm/tools/obj2yaml/ - Format conversion pattern
   - llvm/tools/llvm-objcopy/ - Binary transformation

---

## Appendix A: File Structure Reference

### Proposed Directory Structure

```
llvm/
├── include/llvm/
│   ├── BinaryFormat/
│   │   └── PEF.h                    # PEF format constants
│   ├── Object/
│   │   └── PEF.h                    # PEFObjectFile class
│   └── ObjectYAML/
│       └── PEFYAML.h                # YAML schema (optional)
├── lib/
│   ├── BinaryFormat/
│   │   └── Magic.cpp                # Add PEF magic detection
│   ├── Object/
│   │   ├── Binary.cpp               # Register PEF type
│   │   └── PEFObjectFile.cpp        # PEF parser
│   └── ObjectYAML/
│       └── PEFYAML.cpp              # YAML support (optional)
└── tools/
    ├── elf2pef/
    │   ├── CMakeLists.txt
    │   ├── elf2pef.cpp              # Main driver
    │   ├── PEFWriter.h              # PEF writer class
    │   └── PEFWriter.cpp            # PEF generation
    ├── obj2yaml/
    │   └── pef2yaml.cpp             # PEF → YAML (optional)
    └── yaml2obj/
        └── pef2yaml.cpp             # YAML → PEF (optional)
```

### Estimated Lines of Code

| Component | File | Est. LOC |
|-----------|------|----------|
| Format Definitions | BinaryFormat/PEF.h | 200-300 |
| Object Reader | Object/PEF.h | 100-150 |
| Object Reader | Object/PEFObjectFile.cpp | 800-1200 |
| Magic Detection | BinaryFormat/Magic.cpp | +20 |
| Binary Factory | Object/Binary.cpp | +30 |
| PEF Writer | elf2pef/PEFWriter.h | 100-150 |
| PEF Writer | elf2pef/PEFWriter.cpp | 1200-1800 |
| Main Driver | elf2pef/elf2pef.cpp | 200-300 |
| **Total** | | **2650-4000** |

---

## Appendix B: Relocation Mapping Table

### ELF PowerPC Relocations → PEF Relocations

| ELF Type | Value | Description | PEF Equivalent | Notes |
|----------|-------|-------------|----------------|-------|
| R_PPC_NONE | 0 | No relocation | - | Skip |
| R_PPC_ADDR32 | 1 | 32-bit absolute | RelocLgSetOrBySection | Direct mapping |
| R_PPC_ADDR24 | 2 | 24-bit absolute | RelocBySectDWithSkip | Branch target |
| R_PPC_ADDR16 | 3 | 16-bit absolute | RelocVTable8 | With offset |
| R_PPC_ADDR16_LO | 4 | Low 16 bits | RelocSmByImport | Low half |
| R_PPC_ADDR16_HI | 5 | High 16 bits | RelocSmSetSectC | High half |
| R_PPC_ADDR16_HA | 6 | Adjusted high 16 | RelocSmBySection | HA adjustment |
| R_PPC_ADDR14 | 7 | 14-bit absolute | RelocSmSetSectC | Conditional branch |
| R_PPC_REL24 | 10 | 24-bit PC-relative | RelocTVector12 | Branch and link |
| R_PPC_REL14 | 11 | 14-bit PC-relative | RelocVTable8 | Conditional branch |
| R_PPC_GOT16 | 14 | 16-bit GOT offset | RelocSmByImport | Import reference |
| R_PPC_PLT32 | 27 | 32-bit PLT offset | RelocLgByImport | Import call |

**Note:** This is a simplified mapping. Full implementation requires handling:
- Cross-section references
- Large relocations (>16-bit offsets)
- Import symbol resolution
- Relocation optimization (combining adjacent relocs)

---

## Appendix C: Example Conversion

### Input: Simple C Program

```c
// hello.c
#include <stdio.h>

int main(void) {
    printf("Hello, Classic Mac OS!\n");
    return 0;
}
```

### Step 1: Compile to ELF

```bash
clang -target powerpc-linux-gnu -c hello.c -o hello.o
```

**ELF Sections Created:**
- `.text` - Code (printf call, main function)
- `.rodata` - String constant
- `.data` - (empty)
- `.bss` - (empty)
- `.symtab` - Symbols (main, printf)
- `.rela.text` - Relocations (printf call)

### Step 2: Convert to PEF

```bash
elf2pef hello.o -o hello.pef --entry main
```

**PEF Sections Created:**
1. Section 0 (kindCode)
   - From: `.text`
   - Contains: main(), with relocated printf call

2. Section 1 (kindConstant)
   - From: `.rodata`
   - Contains: "Hello, Classic Mac OS!\n"

3. Section 2 (kindLoader)
   - Synthetic section
   - Contains:
     - Imported libraries: ["InterfaceLib"]
     - Imported symbols: ["printf"]
     - Exported symbols: ["main"]
     - Relocations: [printf call fixup]
     - Entry point: main

**PEF Container Header:**
```
Tag1: "Joy!"
Tag2: "peff"
Architecture: "pwpc"
FormatVersion: 1
SectionCount: 3
InstSectionCount: 2
```

### Step 3: Load on Classic Mac OS

```cpp
// Classic Mac OS code
CFragConnectionID connID;
Ptr mainAddr;
Str255 errName;

GetDiskFragment(
    &fileSpec,           // File location
    0,                   // Offset
    kCFragGoesToEOF,     // Length
    "\phello",           // Fragment name
    kLoadCFrag,          // Load mode
    &connID,             // Connection ID
    &mainAddr,           // Main entry point
    errName              // Error name
);

// Call main
((MainEntryPoint)mainAddr)();
```

---

## Conclusion

Adding PEF support to LLVM is achievable through an external `elf2pef` converter tool. This approach:
- Leverages existing LLVM ELF infrastructure
- Requires minimal LLVM core changes
- Provides a clear migration path to native support if needed
- Can be developed incrementally with testable milestones

The recommended implementation follows proven LLVM patterns and reuses existing binary format handling code. With proper testing and documentation, this will enable LLVM to target Classic Mac OS platforms effectively.
