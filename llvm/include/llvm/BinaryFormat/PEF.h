//===-- PEF.h - PEF (Preferred Executable Format) constants ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines constants and structures for the PEF (Preferred Executable
// Format) object file format used by Mac OS Classic PowerPC executables and
// the Code Fragment Manager.
//
// Reference: "Mac OS Runtime Architectures" (Apple Computer, Inc.)
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_BINARYFORMAT_PEF_H
#define LLVM_BINARYFORMAT_PEF_H

#include "llvm/Support/DataTypes.h"

namespace llvm {
namespace PEF {

/// PEF container tags and version
enum {
  kPEFTag1 = 0x4A6F7921, // 'Joy!' - First magic number
  kPEFTag2 = 0x70656666, // 'peff' - Second magic number
  kPEFVersion = 1,        // Format version
};

/// Architecture types
enum Architecture : uint32_t {
  kPEFPowerPCArch = 0x70777063, // 'pwpc' - PowerPC architecture
  kPEFM68KArch = 0x6D36386B,    // 'm68k' - Motorola 68K architecture
};

/// Section sharing kinds
enum ShareKind : uint8_t {
  kPEFProcessShare = 1,   // Share within process
  kPEFGlobalShare = 4,    // Share globally
  kPEFProtectedShare = 5, // Protected sharing
};

/// Section types
enum SectionKind : uint8_t {
  kPEFCodeSection = 0,            // Executable code
  kPEFUnpackedDataSection = 1,    // Unpacked data
  kPEFPatternDataSection = 2,     // Pattern-initialized data
  kPEFConstantSection = 3,        // Read-only data
  kPEFLoaderSection = 4,          // Loader information
  kPEFDebugSection = 5,           // Debug information
  kPEFExecutableDataSection = 6,  // Executable data
  kPEFExceptionSection = 7,       // Exception information
  kPEFTracebackSection = 8,       // Traceback information
};

/// Symbol classes for exports
enum SymbolClass : uint8_t {
  kPEFCodeSymbol = 0,    // Code symbol
  kPEFDataSymbol = 1,    // Data symbol
  kPEFTVectorSymbol = 2, // Transition vector
  kPEFTOCSymbol = 3,     // Table of contents
  kPEFGlueSymbol = 4,    // Glue symbol
};

/// Import library options
enum ImportLibraryOptions : uint8_t {
  kPEFWeakImportLibMask = 0x40,   // Weak import (can be missing)
  kPEFInitLibBeforeMask = 0x80,   // Initialize before use
};

/// PEF Relocation opcodes
/// These are used to encode relocation instructions in a compact bytecode format
enum RelocOpcode : uint8_t {
  // Basic relocations (0x00-0x1F: with skip count)
  kPEFRelocBySectDWithSkip = 0x00,    // Relocate by data section with skip
  kPEFRelocBySectCWithSkip = 0x01,    // Relocate by code section with skip

  // Section-relative relocations (0x20-0x2F)
  kPEFRelocBySectC = 0x20,            // Relocate by code section offset
  kPEFRelocBySectD = 0x21,            // Relocate by data section offset
  kPEFRelocTVector12 = 0x22,          // 12-byte transition vector
  kPEFRelocTVector8 = 0x23,           // 8-byte transition vector
  kPEFRelocVTable8 = 0x24,            // 8-byte vtable entry
  kPEFRelocImportRun = 0x25,          // Run of imports

  // Position and repeat (0x28-0x2F)
  kPEFRelocSmRepeat = 0x28,           // Small repeat count
  kPEFRelocSmSetSectC = 0x29,         // Set section C
  kPEFRelocSmSetSectD = 0x2A,         // Set section D
  kPEFRelocSmByImport = 0x2B,         // By import (small)

  // Set position (0x48)
  kPEFRelocSetPosition = 0x48,        // Set position (25-bit)

  // Large opcodes (0x50-0x5F)
  kPEFRelocLgByImport = 0x52,         // Relocate by import (large)
  kPEFRelocLgRepeat = 0x58,           // Large repeat count
  kPEFRelocLgSetOrBySection = 0x59,   // Large set or by section
};

/// Relocation instruction composition helpers
/// These inline functions create relocation instruction words

/// Set position (25-bit address split across two instructions)
inline uint16_t composeSetPosition1st(uint32_t Offset) {
  return (kPEFRelocSetPosition << 10) | ((Offset >> 16) & 0x3FF);
}

inline uint16_t composeSetPosition2nd(uint32_t Offset) {
  return Offset & 0xFFFF;
}

/// Relocate by section C (code)
inline uint16_t composeBySectC(uint16_t RunLength) {
  return (kPEFRelocBySectC << 10) | (RunLength & 0x3FF);
}

/// Relocate by section D (data)
inline uint16_t composeBySectD(uint16_t RunLength) {
  return (kPEFRelocBySectD << 10) | (RunLength & 0x3FF);
}

/// Large relocate by import (22-bit index split across two instructions)
inline uint16_t composeLgByImport1st(uint32_t Index) {
  return (kPEFRelocLgByImport << 10) | ((Index >> 16) & 0x3FF);
}

inline uint16_t composeLgByImport2nd(uint32_t Index) {
  return Index & 0xFFFF;
}

/// Hash table parameters
enum {
  kExponentLimit = 16,       // Maximum hash table size: 2^16
  kAverageChainLimit = 10,   // Average chain length for hash sizing
  kPEFHashLengthShift = 16,  // Shift for length in hash slot
  kPEFHashValueMask = 0xFFFF,// Mask for hash value
};

/// Hash slot chain parameters
enum {
  kFirstIndexShift = 0,
  kFirstIndexMask = 0x3FFFF,  // 18 bits
  kChainCountShift = 18,
  kChainCountMask = 0x3FFF,   // 14 bits
};

//===----------------------------------------------------------------------===//
// PEF Binary Structures
//===----------------------------------------------------------------------===//

/// PEF Container Header (40 bytes)
/// This appears at the beginning of every PEF file
struct ContainerHeader {
  uint32_t Tag1;               // Must be 'Joy!' (0x4A6F7921)
  uint32_t Tag2;               // Must be 'peff' (0x70656666)
  uint32_t Architecture;       // 'pwpc' for PowerPC, 'm68k' for 68K
  uint32_t FormatVersion;      // Format version (currently 1)
  uint32_t DateTimeStamp;      // Date/time when created
  uint32_t OldDefVersion;      // Old definition version
  uint32_t OldImpVersion;      // Old implementation version
  uint32_t CurrentVersion;     // Current version
  uint16_t SectionCount;       // Total number of sections
  uint16_t InstSectionCount;   // Number of instantiated sections
  uint32_t ReservedA;          // Reserved, must be zero
};

/// Offset to first section header (immediately after container header)
enum { kFirstSectionHeaderOffset = sizeof(ContainerHeader) };

/// PEF Section Header (40 bytes)
/// One for each section in the container
struct SectionHeader {
  int32_t NameOffset;          // Offset to name in string table (-1 = none)
  uint32_t DefaultAddress;     // Default load address (0 = anywhere)
  uint32_t TotalLength;        // Total section length in memory
  uint32_t UnpackedLength;     // Length of unpacked data
  uint32_t ContainerLength;    // Length in container
  uint32_t ContainerOffset;    // Offset in container
  uint8_t SectionKind;         // Section type (SectionKind enum)
  uint8_t ShareKind;           // Sharing mode (ShareKind enum)
  uint8_t Alignment;           // Alignment (power of 2)
  uint8_t ReservedA;           // Reserved, must be zero
};

/// PEF Loader Info Header (56 bytes)
/// Appears at the beginning of the loader section
struct LoaderInfoHeader {
  int32_t MainSection;               // Section containing main symbol
  uint32_t MainOffset;               // Offset of main symbol
  int32_t InitSection;               // Section containing init function
  uint32_t InitOffset;               // Offset of init function
  int32_t TermSection;               // Section containing term function
  uint32_t TermOffset;               // Offset of term function
  uint32_t ImportedLibraryCount;     // Number of imported libraries
  uint32_t TotalImportedSymbolCount; // Total imported symbols
  uint32_t RelocSectionCount;        // Number of sections with relocations
  uint32_t RelocInstrOffset;         // Offset to relocation instructions
  uint32_t LoaderStringsOffset;      // Offset to loader strings
  uint32_t ExportHashOffset;         // Offset to export hash table
  uint32_t ExportHashTablePower;     // Hash table size (2^N)
  uint32_t ExportedSymbolCount;      // Number of exported symbols
};

/// PEF Imported Library (28 bytes)
/// Describes an imported library dependency
struct ImportedLibrary {
  uint32_t NameOffset;           // Offset to library name in string table
  uint32_t OldImpVersion;        // Oldest compatible implementation version
  uint32_t CurrentVersion;       // Current version
  uint32_t ImportedSymbolCount;  // Number of symbols imported from this library
  uint32_t FirstImportedSymbol;  // Index of first imported symbol
  uint8_t Options;               // Import options (ImportLibraryOptions)
  uint8_t ReservedA;             // Reserved
  uint16_t ReservedB;            // Reserved
};

/// PEF Imported Symbol (4 bytes)
/// Compact representation: class (4 bits) + name offset (28 bits)
struct ImportedSymbol {
  uint32_t ClassAndName; // Symbol class (high 4 bits) + name offset (low 28 bits)
};

/// Helper to extract fields from ImportedSymbol
inline uint8_t getImportedSymbolClass(uint32_t ClassAndName) {
  return ClassAndName >> 28;
}

inline uint32_t getImportedSymbolNameOffset(uint32_t ClassAndName) {
  return ClassAndName & 0x0FFFFFFF;
}

inline uint32_t composeImportedSymbol(uint8_t Class, uint32_t NameOffset) {
  return (static_cast<uint32_t>(Class) << 28) | (NameOffset & 0x0FFFFFFF);
}

/// PEF Loader Relocation Header (12 bytes)
/// Describes relocations for one section
struct LoaderRelocationHeader {
  uint16_t SectionIndex;      // Section to be relocated
  uint16_t ReservedA;         // Reserved
  uint32_t RelocCount;        // Number of relocation instructions
  uint32_t FirstRelocOffset;  // Offset to first relocation instruction
};

/// PEF Exported Symbol (10 bytes)
/// Describes an exported symbol
struct ExportedSymbol {
  uint32_t ClassAndName;   // Symbol class (8 bits) + name offset (24 bits)
  uint32_t SymbolValue;    // Symbol value (offset in section)
  int16_t SectionIndex;    // Section index (-1 = absolute, -2 = undefined)
};

/// Helper to extract fields from ExportedSymbol
inline uint8_t getExportedSymbolClass(uint32_t ClassAndName) {
  return ClassAndName >> 24;
}

inline uint32_t getExportedSymbolNameOffset(uint32_t ClassAndName) {
  return ClassAndName & 0x00FFFFFF;
}

inline uint32_t composeExportedSymbol(uint8_t Class, uint32_t NameOffset) {
  return (static_cast<uint32_t>(Class) << 24) | (NameOffset & 0x00FFFFFF);
}

/// PEF Hash Slot Entry (4 bytes)
/// Used in export hash table
struct HashSlotEntry {
  uint32_t Value; // Chain count (14 bits) + first index (18 bits)
};

inline uint32_t getHashSlotChainCount(uint32_t Value) {
  return (Value >> kChainCountShift) & kChainCountMask;
}

inline uint32_t getHashSlotFirstIndex(uint32_t Value) {
  return (Value >> kFirstIndexShift) & kFirstIndexMask;
}

inline uint32_t composeHashSlot(uint32_t ChainCount, uint32_t FirstIndex) {
  return ((ChainCount & kChainCountMask) << kChainCountShift) |
         ((FirstIndex & kFirstIndexMask) << kFirstIndexShift);
}

/// PEF Hash Chain Entry (4 bytes)
/// Links export symbols in hash chains
struct HashChainEntry {
  uint32_t Value; // Name length (16 bits) + hash value (16 bits)
};

inline uint16_t getHashChainNameLength(uint32_t Value) {
  return Value >> kPEFHashLengthShift;
}

inline uint16_t getHashChainHashValue(uint32_t Value) {
  return Value & kPEFHashValueMask;
}

inline uint32_t composeHashChain(uint16_t NameLength, uint16_t HashValue) {
  return (static_cast<uint32_t>(NameLength) << kPEFHashLengthShift) |
         (HashValue & kPEFHashValueMask);
}

} // end namespace PEF
} // end namespace llvm

#endif // LLVM_BINARYFORMAT_PEF_H
