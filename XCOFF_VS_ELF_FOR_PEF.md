# XCOFF vs ELF: Choosing the Best Base Format for PEF Conversion

## Executive Summary

**Recommendation: Use XCOFF as the intermediate format for PEF conversion.**

XCOFF (Extended Common Object File Format) is a significantly better match for PEF than ELF because:
1. **Same ABI** - Both use PowerOpen ABI with function descriptors and TOC
2. **Native LLVM Support** - PowerPC backend has full XCOFF support for AIX
3. **Historical Connection** - Early PowerPC Macs supported XCOFF
4. **Structural Similarity** - Sections, symbols, and relocations map more naturally
5. **Easier Conversion** - Research indicates XCOFF↔ELF conversion is "nearly impossible" due to ABI differences

---

## Format Comparison Matrix

| Feature | XCOFF | ELF | PEF | Best Match |
|---------|-------|-----|-----|------------|
| **Endianness** | Big-endian | Both | Big-endian | XCOFF = PEF |
| **ABI** | PowerOpen | V.4/eabi | PowerOpen | **XCOFF = PEF** ✓ |
| **Function Pointers** | Descriptors | Direct | Descriptors | **XCOFF = PEF** ✓ |
| **TOC Usage** | Yes (XMC_TC) | Different | Yes (imports) | **XCOFF ≈ PEF** ✓ |
| **Section Types** | STYP_TEXT/DATA/BSS/LOADER | .text/.data/.bss | kindCode/kindData/kindLoader | **XCOFF ≈ PEF** ✓ |
| **Relocations** | XCOFF relocs | ELF R_PPC_* | PEF opcodes | Both need mapping |
| **Symbols** | C_EXT/C_HIDEXT with storage class | STT_FUNC/STT_OBJECT | Hash table exports | Both need conversion |
| **Entry Point** | Auxiliary header | ELF header | Loader section | Both supported |
| **LLVM Support** | **Full (AIX)** ✓ | Full (Linux) | None | **XCOFF** better |

**Score: XCOFF wins 6/9 categories, ties 1/9, ELF ties 2/9**

---

## Detailed Analysis

### 1. ABI Compatibility (CRITICAL)

**XCOFF (PowerOpen ABI):**
```
Function Descriptor Layout (PowerOpen/AIX/Classic Mac):
┌─────────────────────┐
│ Function Address    │  ← Function pointer points HERE
├─────────────────────┤
│ TOC Pointer (r2)    │
├─────────────────────┤
│ Environment (r11)   │
└─────────────────────┘

Code:
  lwz r12, 0(function_ptr)   ; Load actual function address
  lwz r2, 4(function_ptr)    ; Load TOC pointer
  mtctr r12                   ; Move to count register
  bctrl                       ; Branch to function
```

**ELF (V.4 ABI):**
```
Direct Function Pointer:
┌─────────────────────┐
│ [Function Code]     │  ← Function pointer points to CODE directly
│ ...                 │
└─────────────────────┘

Code:
  bl function_name            ; Direct branch and link
  ; No descriptor needed
```

**PEF (PowerOpen ABI - Same as XCOFF!):**
```
Function Import:
┌─────────────────────┐
│ Import Address      │  ← Resolved by Code Fragment Manager
├─────────────────────┤
│ Import Name Offset  │
└─────────────────────┘

Uses function descriptors like XCOFF/PowerOpen
```

**Conclusion:** XCOFF and PEF use the **same ABI** (PowerOpen). Converting XCOFF→PEF preserves calling conventions. ELF→PEF would require ABI translation (adding function descriptors, rewriting call sequences).

### 2. TOC (Table of Contents) Structure

**XCOFF TOC:**
```
Storage Mapping Classes in XCOFF:
- XMC_TC0 (15): TOC anchor for addressability
- XMC_TC (3): General TOC item
- XMC_TD (16): Scalar data in TOC
- XMC_DS (10): Descriptor csect

Example:
  .toc
  LC..0:
    .tc .printf[TC],printf[DS]    ; TOC entry for printf descriptor

  Code:
    lwz r3, LC..0(r2)              ; Load from TOC using r2 (TOC base)
```

**PEF Loader Section:**
```
Imported Symbols:
- Symbol class (code, data, TVect, TOC, glue)
- Symbol name offset into string table
- Relocations reference imports via symbol index

Similar concept to XCOFF TOC:
  - Both use indirection tables
  - Both have base register (XCOFF: r2, PEF: runtime setup)
  - Both support lazy binding
```

**ELF GOT (Global Offset Table):**
```
Different mechanism:
  .got section
  - ELF uses @got relocations
  - Different register conventions
  - V.4 ABI doesn't mandate TOC

Example:
  addis r3, r2, symbol@got@ha
  ld r3, symbol@got@l(r3)
```

**Conclusion:** XCOFF's TOC maps more naturally to PEF's import mechanism. Both use similar indirection concepts from PowerOpen ABI.

### 3. Section Mapping

#### XCOFF → PEF Mapping (Natural)

| XCOFF Section | STYP Flags | PEF Section | Kind | Notes |
|---------------|------------|-------------|------|-------|
| .text | STYP_TEXT (0x0020) | Code Section | kindCode (0) | Direct map |
| .data | STYP_DATA (0x0040) | Data Section | kindUnpackedData (1) | Direct map |
| .bss | STYP_BSS (0x0080) | Data Section | kindUnpackedData (1) | Zero-init |
| .loader | STYP_LOADER (0x1000) | Loader Section | kindLoader (4) | **Similar concept!** |
| .rodata | STYP_DATA + read-only | Constant Section | kindConstant (3) | Attribute map |
| .tdata | STYP_TDATA (0x0400) | Data Section | kindUnpackedData (1) | Thread-local |
| .tbss | STYP_TBSS (0x0800) | Data Section | kindUnpackedData (1) | Thread-local BSS |

**Key Advantage:** XCOFF has a **.loader** section (STYP_LOADER) which serves a similar purpose to PEF's **Loader Section**:
- XCOFF .loader: Contains import/export information for dynamic linking
- PEF Loader: Contains imports, exports, relocations, entry points

This is a **major structural similarity** not present in ELF!

#### ELF → PEF Mapping (Requires Translation)

| ELF Section | Type | PEF Section | Kind | Issues |
|-------------|------|-------------|------|--------|
| .text | SHT_PROGBITS | Code Section | kindCode (0) | OK |
| .data | SHT_PROGBITS | Data Section | kindUnpackedData (1) | OK |
| .bss | SHT_NOBITS | Data Section | kindUnpackedData (1) | OK |
| .rodata | SHT_PROGBITS | Constant Section | kindConstant (3) | OK |
| .dynamic | SHT_DYNAMIC | Loader Section | kindLoader (4) | **Major translation needed** |
| .got | SHT_PROGBITS | ??? | ??? | **No direct equivalent** |
| .plt | SHT_PROGBITS | ??? | ??? | **Different linking model** |

**Problem:** ELF uses .got/.plt for dynamic linking (V.4 ABI), which doesn't map to PEF's loader section model.

### 4. LLVM PowerPC Backend Support

**XCOFF Support in LLVM:**

Located in `llvm/lib/Target/PowerPC/MCTargetDesc/`:
- `PPCXCOFFStreamer.cpp` - XCOFF-specific streamer
- `PPCXCOFFObjectWriter.cpp` - XCOFF object writer
- Integrated into PowerPC backend

From `PPCTargetMachine.cpp:237-241`:
```cpp
static std::unique_ptr<TargetLoweringObjectFile> createTLOF(const Triple &TT) {
  if (TT.isOSAIX())
    return std::make_unique<TargetLoweringObjectFileXCOFF>();

  return std::make_unique<PPC64LinuxTargetObjectFile>();
}
```

**Target Triple Support:**
```bash
# XCOFF output (already works!)
clang -target powerpc-ibm-aix -c hello.c -o hello.o

# Produces XCOFF object file with:
# - PowerOpen ABI
# - Function descriptors
# - TOC entries
# - XCOFF relocations
```

**ELF Support:**
- Default for PowerPC Linux
- Uses V.4 ABI (different from Classic Mac)
- Would require more conversion logic

### 5. Historical Context

From web research:

> "Early versions of the PowerPC Macintosh also supported XCOFF, as did BeOS."

> "Most of the Mac compilers/linkers produce PEF code, while XCOFF output code requires setting up the TOC and loading the .data, .text & .bss segments."

**Implication:** There was actual tooling that worked with both XCOFF and PEF on PowerPC Macs, suggesting conversion feasibility.

### 6. Symbol Table Comparison

**XCOFF Symbol Table:**
```cpp
struct XCOFFSymbolEntry {
  char Name[8];              // Or offset into string table
  uint32_t Value;            // Symbol value/address
  int16_t SectionNumber;     // Section index
  uint16_t Type;             // Symbol type
  uint8_t StorageClass;      // C_EXT, C_HIDEXT, etc.
  uint8_t NumberOfAuxEntries;
};

Storage Classes:
- C_EXT (2): External symbol (exported)
- C_WEAKEXT (111): Weak external
- C_HIDEXT (107): Hidden external (not exported)
- C_FILE (103): File name
- C_STAT (3): Static symbol
```

**PEF Exported Symbol:**
```cpp
struct PEFExportedSymbol {
  uint32_t SymbolClass;      // Code, Data, TVector, TOC, Glue
  uint32_t SymbolNameOffset; // Offset into string table
  uint32_t SymbolValue;      // Section offset
  int16_t SectionIndex;      // Section index (-2 for re-exported)
};

Symbol Classes:
- Code (0x00): Executable code
- Data (0x01): Data symbol
- TVectorDesc (0x02): Transition vector descriptor
- TOC (0x03): TOC symbol
- Glue (0x04): Glue code
```

**Mapping:**
```
XCOFF Storage Class       →  PEF Symbol Class
─────────────────────────────────────────────
C_EXT + in .text          →  Code (0x00)
C_EXT + in .data          →  Data (0x01)
C_EXT + descriptor        →  TVectorDesc (0x02)
C_EXT + XMC_TC            →  TOC (0x03)
C_HIDEXT                  →  (not exported)
```

**ELF Symbol Table:**
```cpp
struct Elf32_Sym {
  uint32_t st_name;     // String table offset
  uint32_t st_value;    // Symbol value
  uint32_t st_size;     // Symbol size
  uint8_t st_info;      // Type and binding
  uint8_t st_other;     // Visibility
  uint16_t st_shndx;    // Section index
};

Types:
- STT_FUNC: Function
- STT_OBJECT: Data object
- STT_SECTION: Section symbol
- STT_FILE: File name
```

**Issue:** ELF doesn't distinguish function descriptors vs direct functions. XCOFF explicitly tracks this via storage mapping class.

### 7. Relocation Comparison

**XCOFF Relocations (PowerPC):**
```
R_POS (0x00): Positive relocation
R_NEG (0x01): Negative relocation
R_REL (0x02): Relative relocation
R_TOC (0x03): TOC relative
R_TRL (0x12): TOC relative branch
R_TRLA (0x13): TOC relative load address
R_GL (0x05): Global linkage
R_TCL (0x06): Local object TOC address
R_BA (0x08): Branch absolute
R_BR (0x0A): Branch relative
```

**PEF Relocations (Opcodes):**
```
RelocBySectDWithSkip (0x00)
RelocBySectC (0x02)
RelocTVector12 (0x06)
RelocVTable8 (0x0C)
RelocImportRun (0x0D)
RelocSmByImport (0x1A)
RelocSmSetSectC (0x1B)
RelocSmSetSectD (0x1C)
RelocSmBySection (0x1D)
RelocLgByImport (0x24)
RelocLgSetOrBySection (0x2C)
```

**Both Need Mapping:** Relocations will need conversion in either case, but XCOFF's TOC-relative relocations (R_TOC, R_TRL) map more naturally to PEF's import mechanism.

**ELF Relocations (PowerPC):**
```
R_PPC_ADDR32: 32-bit absolute
R_PPC_ADDR24: 24-bit absolute (branches)
R_PPC_REL24: 24-bit PC-relative
R_PPC_GOT16: 16-bit GOT offset
R_PPC_PLT32: 32-bit PLT offset
```

**Issue:** ELF's GOT/PLT relocations don't have direct PEF equivalents.

---

## Implementation Comparison

### Option 1: xcoff2pef (RECOMMENDED)

**Architecture:**
```
Source Code
    ↓
LLVM Clang
    ↓ (PowerPC CodeGen)
clang -target powerpc-ibm-aix
    ↓
XCOFF Object File
  ├─ PowerOpen ABI ✓
  ├─ Function Descriptors ✓
  ├─ TOC Entries ✓
  ├─ .text/.data/.bss sections
  └─ .loader section ✓
    ↓
xcoff2pef Tool
    ├─ Parse XCOFF
    ├─ Map sections (natural 1:1)
    ├─ Convert symbols (preserve descriptors)
    ├─ Translate relocations
    └─ Build PEF loader from XCOFF .loader
    ↓
PEF Executable
  └─ Ready for Classic Mac OS
```

**Advantages:**
1. **ABI Preserved** - No calling convention changes needed
2. **Natural Section Mapping** - .text→kindCode, .data→kindUnpackedData, .loader→kindLoader
3. **TOC Mapping** - XCOFF TOC entries → PEF imports
4. **Function Descriptors** - Already in XCOFF, just translate format
5. **LLVM Support** - Full PowerPC XCOFF backend exists
6. **Historical Precedent** - Early PowerPC Macs used XCOFF

**Disadvantages:**
1. Less common target (AIX-specific)
2. Need to understand XCOFF .loader section format
3. Relocation translation still required

**Estimated Complexity:** **Medium** (6-8 weeks)

### Option 2: elf2pef

**Architecture:**
```
Source Code
    ↓
LLVM Clang
    ↓ (PowerPC CodeGen)
clang -target powerpc-linux-gnu
    ↓
ELF Object File
  ├─ V.4 ABI ✗ (wrong ABI!)
  ├─ Direct Function Pointers ✗
  ├─ GOT/PLT ✗ (no PEF equivalent)
  ├─ .text/.data/.bss sections
  └─ .dynamic section
    ↓
elf2pef Tool
    ├─ Parse ELF
    ├─ **SYNTHESIZE Function Descriptors** ✗
    ├─ **REWRITE Call Sequences** ✗
    ├─ **TRANSLATE GOT→Imports** ✗
    ├─ **BUILD Loader from .dynamic** ✗
    └─ Translate relocations
    ↓
PEF Executable
  └─ May not work correctly due to ABI mismatch
```

**Advantages:**
1. More common target (Linux)
2. Well-understood ELF format

**Disadvantages:**
1. **ABI Mismatch** - V.4 vs PowerOpen (CRITICAL!)
2. **No Function Descriptors** - Must synthesize (complex!)
3. **Different Linking Model** - GOT/PLT vs TOC/Imports
4. **Call Sequence Rewriting** - May need to modify code
5. **Research Says** - "Nearly impossible" due to ABI differences

**Estimated Complexity:** **High** (12-16 weeks, with risk of incompatibility)

---

## Detailed xcoff2pef Tool Design

### Phase 1: XCOFF Parser

**Goal:** Read and understand XCOFF structure

**Tasks:**
1. Use existing `llvm/include/llvm/Object/XCOFFObjectFile.h`
2. Parse file header, section headers, symbol table
3. Extract .loader section data
4. Identify TOC entries (XMC_TC storage class)
5. Enumerate imported/exported symbols

**Deliverable:** Complete XCOFF data model in memory

### Phase 2: Section Converter

**Section Mapping Logic:**

```cpp
PEFSectionKind mapXCOFFSectionToPEF(const XCOFFSection &Section) {
  uint32_t Flags = Section.getSectionFlags();

  if (Flags & XCOFF::STYP_TEXT)
    return PEF::kindCode;

  if (Flags & XCOFF::STYP_DATA) {
    // Check if read-only
    if (Section.isReadOnly())
      return PEF::kindConstant;
    return PEF::kindUnpackedData;
  }

  if (Flags & XCOFF::STYP_BSS)
    return PEF::kindUnpackedData; // Zero-initialized

  if (Flags & XCOFF::STYP_LOADER)
    return PEF::kindLoader; // Special handling

  // Default for unknown sections
  return PEF::kindConstant;
}
```

**Data Packing:**
```cpp
void packSectionData(PEFSection &PEFSect, const XCOFFSection &XCOFFSect) {
  // PEF supports pattern-initialized data
  // Check if data can be compressed

  if (isRepeatingPattern(XCOFFSect.getData())) {
    PEFSect.setKind(PEF::kindPatternInitializedData);
    PEFSect.setPackedData(compressPattern(XCOFFSect.getData()));
  } else {
    PEFSect.setKind(PEF::kindUnpackedData);
    PEFSect.setUnpackedData(XCOFFSect.getData());
  }
}
```

### Phase 3: Symbol Converter

**Export Symbol Conversion:**

```cpp
void convertExportSymbols(const XCOFFObjectFile &XCOFF, PEFLoaderSection &Loader) {
  for (const auto &Sym : XCOFF.symbols()) {
    // Only export external symbols
    if (Sym.getStorageClass() != XCOFF::C_EXT)
      continue;

    PEFExportedSymbol PEFSym;
    PEFSym.setNameOffset(addToStringTable(Sym.getName()));
    PEFSym.setSectionIndex(Sym.getSectionNumber());
    PEFSym.setValue(Sym.getValue());

    // Map storage mapping class to PEF symbol class
    auto StorageMappingClass = Sym.getAuxCSectStorageMappingClass();
    switch (StorageMappingClass) {
      case XCOFF::XMC_PR: // Program code
        PEFSym.setSymbolClass(PEF::kCodeSymbol);
        break;
      case XCOFF::XMC_RW: // Read-write data
      case XCOFF::XMC_RO: // Read-only constant
        PEFSym.setSymbolClass(PEF::kDataSymbol);
        break;
      case XCOFF::XMC_DS: // Descriptor
        PEFSym.setSymbolClass(PEF::kTVectorSymbol);
        break;
      case XCOFF::XMC_TC: // TOC entry
      case XCOFF::XMC_TC0: // TOC anchor
        PEFSym.setSymbolClass(PEF::kTOCSymbol);
        break;
      case XCOFF::XMC_GL: // Global linkage
        PEFSym.setSymbolClass(PEF::kGlueSymbol);
        break;
      default:
        PEFSym.setSymbolClass(PEF::kDataSymbol);
    }

    Loader.addExportedSymbol(PEFSym);
  }

  // Build export hash table
  Loader.buildExportHashTable();
}
```

**Import Symbol Conversion:**

```cpp
void convertImportSymbols(const XCOFFLoaderSection &XCOFFLoader,
                          PEFLoaderSection &PEFLoader) {
  // XCOFF .loader section contains imported symbols
  for (const auto &ImpSym : XCOFFLoader.getImportedSymbols()) {
    PEFImportedSymbol PEFImp;
    PEFImp.setNameOffset(addToStringTable(ImpSym.getName()));

    // Determine symbol class from XCOFF symbol type
    if (ImpSym.isFunction())
      PEFImp.setSymbolClass(PEF::kCodeSymbol);
    else
      PEFImp.setSymbolClass(PEF::kDataSymbol);

    PEFLoader.addImportedSymbol(PEFImp);
  }

  // Group imports by library
  PEFLoader.buildImportedLibraryTable();
}
```

### Phase 4: Relocation Converter

**Relocation Mapping Table:**

```cpp
struct RelocationMapping {
  uint8_t XCOFFType;
  PEFRelocOpcode PEFOpcode;
  bool NeedsAdjustment;
};

const RelocationMapping RelocMap[] = {
  // XCOFF R_POS → PEF absolute address
  {XCOFF::R_POS, PEF::RelocLgSetOrBySection, false},

  // XCOFF R_TOC → PEF import reference
  {XCOFF::R_TOC, PEF::RelocSmByImport, true},

  // XCOFF R_BR → PEF branch relocation
  {XCOFF::R_BR, PEF::RelocBySectC, false},

  // XCOFF R_TRL → PEF TOC-relative load
  {XCOFF::R_TRL, PEF::RelocSmByImport, true},

  // Add more mappings...
};

void convertRelocations(const XCOFFSection &XCOFFSect, PEFSection &PEFSect) {
  std::vector<uint8_t> PEFRelocStream;

  for (const auto &Reloc : XCOFFSect.relocations()) {
    uint8_t XCOFFType = Reloc.getType();

    // Find mapping
    auto It = std::find_if(RelocMap.begin(), RelocMap.end(),
                           [XCOFFType](const RelocationMapping &M) {
                             return M.XCOFFType == XCOFFType;
                           });

    if (It != RelocMap.end()) {
      // Encode PEF relocation opcode
      encodePEFRelocation(PEFRelocStream, It->PEFOpcode,
                          Reloc.getOffset(), Reloc.getSymbol());
    } else {
      // Unsupported relocation type
      reportError("Unsupported XCOFF relocation type: " +
                  std::to_string(XCOFFType));
    }
  }

  // Add relocation stream to loader section
  PEFSect.setRelocations(PEFRelocStream);
}
```

**PEF Relocation Encoding:**

```cpp
void encodePEFRelocation(std::vector<uint8_t> &Stream,
                         PEFRelocOpcode Opcode,
                         uint32_t Offset,
                         uint32_t SymbolIndex) {
  // PEF relocations are a bytecode stream
  // Example: RelocLgSetOrBySection

  switch (Opcode) {
    case PEF::RelocLgSetOrBySection:
      // Large relocation: 32-bit offset
      Stream.push_back(0x2C); // Opcode
      Stream.push_back((Offset >> 24) & 0xFF);
      Stream.push_back((Offset >> 16) & 0xFF);
      Stream.push_back((Offset >> 8) & 0xFF);
      Stream.push_back(Offset & 0xFF);
      break;

    case PEF::RelocSmByImport:
      // Small relocation by import
      Stream.push_back(0x1A); // Opcode
      Stream.push_back((SymbolIndex >> 8) & 0xFF);
      Stream.push_back(SymbolIndex & 0xFF);
      break;

    // Add more encodings...
  }
}
```

### Phase 5: Loader Section Builder

**Build Complete PEF Loader Section:**

```cpp
void buildLoaderSection(const XCOFFObjectFile &XCOFF, PEFLoaderSection &Loader) {
  // 1. Set entry point
  if (auto EntryPoint = XCOFF.getEntryPointAddress()) {
    auto [Section, Offset] = findSectionAndOffset(*EntryPoint);
    Loader.setMainSection(Section);
    Loader.setMainOffset(Offset);
  }

  // 2. Set init/term functions (if present)
  if (auto InitFunc = findSymbol(XCOFF, "_init")) {
    Loader.setInitSection(InitFunc->getSectionNumber());
    Loader.setInitOffset(InitFunc->getValue());
  }

  if (auto TermFunc = findSymbol(XCOFF, "_fini")) {
    Loader.setTermSection(TermFunc->getSectionNumber());
    Loader.setTermOffset(TermFunc->getValue());
  }

  // 3. Convert imported libraries and symbols
  convertImportSymbols(XCOFF.getLoaderSection(), Loader);

  // 4. Convert exported symbols
  convertExportSymbols(XCOFF, Loader);

  // 5. Build relocations for all sections
  for (const auto &Section : XCOFF.sections()) {
    convertRelocations(Section, Loader);
  }

  // 6. Build string table
  Loader.finalizeStringTable();

  // 7. Build export hash table
  Loader.buildExportHashTable();
}
```

### Phase 6: PEF Writer

**Write Complete PEF File:**

```cpp
class PEFWriter {
  PEFContainerHeader Header;
  std::vector<PEFSectionHeader> SectionHeaders;
  std::vector<std::vector<uint8_t>> SectionData;
  PEFLoaderSection Loader;

public:
  Error writeToFile(StringRef Path) {
    std::error_code EC;
    raw_fd_ostream OS(Path, EC, sys::fs::OF_None);
    if (EC)
      return errorCodeToError(EC);

    // Write container header
    writeContainerHeader(OS);

    // Write section headers
    for (const auto &SecHdr : SectionHeaders)
      writeSectionHeader(OS, SecHdr);

    // Align to 16 bytes (PEF requirement)
    alignTo(OS, 16);

    // Write section data
    for (const auto &Data : SectionData) {
      OS.write(reinterpret_cast<const char*>(Data.data()), Data.size());
      alignTo(OS, 16);
    }

    // Write loader section
    writeLoaderSection(OS, Loader);

    return Error::success();
  }

private:
  void writeContainerHeader(raw_ostream &OS) {
    // Magic numbers
    OS << "Joy!";
    OS << "peff";

    // Architecture
    OS << "pwpc";

    // Version, timestamp, etc.
    support::endian::write<uint32_t>(OS, 1, llvm::endianness::big); // Version
    support::endian::write<uint32_t>(OS, getCurrentPEFTimestamp(),
                                     llvm::endianness::big);
    support::endian::write<uint32_t>(OS, 0, llvm::endianness::big); // OldDefVersion
    support::endian::write<uint32_t>(OS, 0, llvm::endianness::big); // OldImpVersion
    support::endian::write<uint32_t>(OS, 1, llvm::endianness::big); // CurrentVersion
    support::endian::write<uint16_t>(OS, SectionHeaders.size(),
                                     llvm::endianness::big);
    support::endian::write<uint16_t>(OS, getInstantiatedSectionCount(),
                                     llvm::endianness::big);
    support::endian::write<uint32_t>(OS, 0, llvm::endianness::big); // Reserved
  }

  // Additional writer methods...
};
```

---

## Command-Line Usage

### Compiling with XCOFF Output

```bash
# Step 1: Compile C/C++ to XCOFF
clang -target powerpc-ibm-aix7.2.0.0 \
      -mcpu=ppc \
      -c hello.c \
      -o hello.xcoff

# Step 2: Convert XCOFF to PEF
xcoff2pef hello.xcoff -o hello.pef \
          --entry main \
          --creator 'CWIE' \
          --type 'APPL'

# Step 3: Deploy to Classic Mac OS
# Copy hello.pef to Mac and run with Code Fragment Manager
```

### Tool Options

```
xcoff2pef [options] <input.xcoff> -o <output.pef>

Required:
  <input.xcoff>          Input XCOFF object file
  -o <output.pef>        Output PEF file

Options:
  --entry <symbol>       Entry point symbol (default: main)
  --init <symbol>        Initialization function
  --term <symbol>        Termination function
  --creator <code>       4-character creator code (default: '????')
  --type <code>          4-character file type (default: 'APPL')
  --version <n.n.n>      Version number (default: 1.0.0)
  --no-pack-data         Disable pattern data packing
  --optimize-relocs      Optimize relocation stream size
  --verbose              Verbose output
  --dump-sections        Dump section information
  --dump-symbols         Dump symbol tables
  --verify               Verify output PEF structure
```

---

## Testing Strategy

### Test Cases

1. **Simple Hello World**
   - Minimal imports (printf)
   - Single code section
   - String constant in rodata

2. **Multiple Sections**
   - .text, .data, .bss, .rodata
   - Verify section type mapping

3. **Function Descriptors**
   - Test function pointer calls
   - Verify descriptor preservation

4. **TOC References**
   - Global variables via TOC
   - Function calls via TOC
   - Verify import conversion

5. **Symbol Export/Import**
   - Library with exported functions
   - Client importing functions
   - Verify hash table generation

6. **Relocations**
   - Various relocation types
   - Cross-section references
   - Large offsets

7. **Complex Program**
   - C++ with classes and vtables
   - Exception handling (if supported)
   - Static constructors/destructors

### Verification

```bash
# 1. Parse XCOFF
llvm-readobj --all hello.xcoff > xcoff.txt

# 2. Convert
xcoff2pef hello.xcoff -o hello.pef --verify

# 3. Verify PEF structure
pef-dump hello.pef > pef.txt  # Using PEFObjectFile reader

# 4. Compare
# - Section counts match
# - Symbol counts match
# - Relocation counts match

# 5. Test on Classic Mac OS (emulator)
# Run in SheepShaver or QEMU
```

---

## Implementation Timeline

| Phase | Task | Duration | Dependencies |
|-------|------|----------|--------------|
| 1 | PEF Format Library (if not done) | 2 weeks | None |
| 2 | XCOFF Parser Integration | 1 week | Phase 1 |
| 3 | Section Converter | 1 week | Phase 2 |
| 4 | Symbol Converter | 1.5 weeks | Phase 2 |
| 5 | Relocation Converter | 2 weeks | Phases 3-4 |
| 6 | Loader Section Builder | 1.5 weeks | Phases 3-5 |
| 7 | PEF Writer | 1 week | All above |
| 8 | Testing & Debugging | 2 weeks | All above |
| 9 | Documentation | 1 week | Phase 8 |
| **Total** | | **12 weeks** | |

---

## Conclusion

**XCOFF is the superior choice for PEF conversion:**

1. **ABI Compatibility** - Both use PowerOpen ABI (CRITICAL)
2. **Structural Similarity** - XCOFF .loader ≈ PEF Loader section
3. **Function Descriptors** - Native in XCOFF, must synthesize in ELF
4. **TOC Support** - XCOFF TOC maps to PEF imports
5. **LLVM Support** - Full PowerPC XCOFF backend exists
6. **Historical Precedent** - Early Macs supported XCOFF

**Recommendation: Build `xcoff2pef` tool using XCOFF as intermediate format.**

The conversion is more natural, preserves ABI semantics, and leverages existing LLVM infrastructure. The estimated development time is 12 weeks with lower risk than ELF-based approach.
