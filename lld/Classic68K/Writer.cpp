//===- Writer.cpp - Classic 68K application writer ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Writer.h"
#include "CodeSegment.h"
#include "Config.h"
#include "DataSection.h"
#include "ELFReader.h"
#include "JumpTable.h"
#include "ResourceWriter.h"
#include "SizeResource.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace lld::classic68k {

// Startup prologue for Classic 68K applications
// This is prepended to the user's code and becomes the entry point (__start)
//
// __start:
//     link    a6, #0              ; 4E56 0000 - Create stack frame
//     bsr.w   _main               ; 6100 XXXX - PC-relative call to main (68000 compatible)
//     move.w  d0, -(sp)           ; 3F00 - Push return code (low word)
//     _ExitToShell                ; A9F4 - Trap to exit application
//     unlk    a6                  ; 4E5E - (unreachable) cleanup frame
//     rts                         ; 4E75 - (unreachable) return
//
// Total: 16 bytes
// The displacement at offset 6-7 gets patched with PC-relative offset to main()
//
// BSR.W encoding: 0x6100 + 16-bit signed displacement
// Displacement = target - (PC after reading opcode word)
//              = target - (instruction_address + 2)
//
static const uint8_t startupPrologue[] = {
    0x4E, 0x56, 0x00, 0x00,  // LINK A6, #0
    0x61, 0x00,              // BSR.W - displacement follows (2 bytes)
    0x00, 0x00,              // [placeholder for 16-bit displacement - patched later]
    0x3F, 0x00,              // MOVE.W D0, -(SP)
    0xA9, 0xF4,              // _ExitToShell trap
    0x4E, 0x5E,              // UNLK A6
    0x4E, 0x75,              // RTS
};

static constexpr size_t STARTUP_SIZE = sizeof(startupPrologue);

bool Writer::link() {
  if (inputs.empty()) {
    errorMsg = "no input files";
    return false;
  }

  // Collect all code and data from input files
  std::vector<uint8_t> userCode;  // User's compiled code (main, etc.)
  std::vector<uint8_t> allData;
  uint32_t bssSize = 0;

  // Collect function symbols for jump table
  std::vector<std::pair<std::string, uint32_t>> functions; // name, offset in userCode

  for (ELFReader *reader : inputs) {
    // Get text section
    const ELFSection *text = reader->getTextSection();
    if (text && !text->data.empty()) {
      // Record function offsets (relative to start of userCode)
      for (const auto &sym : reader->getSymbols()) {
        if (sym.isFunction() && sym.isGlobal()) {
          // Offset in combined user code = current code size + symbol value
          uint32_t offset = userCode.size() + sym.value;
          functions.push_back({sym.name, offset});
        }
      }
      // Append code
      userCode.insert(userCode.end(), text->data.begin(), text->data.end());
    }

    // Get data section
    const ELFSection *data = reader->getDataSection();
    if (data && !data->data.empty()) {
      allData.insert(allData.end(), data->data.begin(), data->data.end());
    }

    // Get BSS size
    const ELFSection *bss = reader->getBSSSection();
    if (bss) {
      bssSize += bss->size;
    }
  }

  // Find entry point (main)
  const ELFSymbol *entrySym = nullptr;
  for (ELFReader *reader : inputs) {
    entrySym = reader->findSymbol(config->entrySymbol);
    if (entrySym)
      break;
  }

  if (!entrySym) {
    errorMsg = "entry point not found: " + config->entrySymbol;
    return false;
  }

  // Find main() offset in user code
  uint32_t mainOffsetInUserCode = 0;
  for (const auto &[name, offset] : functions) {
    if (name == config->entrySymbol) {
      mainOffsetInUserCode = offset;
      break;
    }
  }

  // Build the final code with startup prologue prepended
  // Layout: [startup prologue (18 bytes)] [user code (main, etc.)]
  std::vector<uint8_t> allCode;
  allCode.reserve(STARTUP_SIZE + userCode.size());

  // Copy startup prologue
  allCode.insert(allCode.end(), startupPrologue,
                 startupPrologue + STARTUP_SIZE);

  // Append user code
  allCode.insert(allCode.end(), userCode.begin(), userCode.end());

  // Patch the JSR address in startup prologue to point to main()
  // main() is at: CODE segment header (4 bytes) + startup size + mainOffsetInUserCode
  // But we need absolute address in memory, which depends on where code is loaded.
  // For classic 68K, we use a PC-relative or A5-relative scheme.
  //
  // Actually, for a simple single-segment app, we can compute the offset from
  // the start of CODE 1's code area:
  //   main address = (offset from CODE 1 start after header)
  //
  // The JSR absolute long needs a 32-bit address. For Classic Mac apps,
  // this address will be patched by the Segment Loader when the jump table
  // entry is loaded. But since we're calling main() directly from startup
  // (not through the jump table), we need to use PC-relative addressing instead.
  //
  // Let's change to BSR (branch subroutine) with PC-relative offset instead
  // of JSR absolute, since BSR.W can reach ±32KB which is enough for small apps.
  //
  // Actually, let's keep JSR and compute the absolute offset. The startup code
  // runs within CODE 1 itself, so we know the relative positions. The Segment
  // Loader sets up the memory layout, and the jump table entry that jumps to
  // __start will have the correct absolute address patched. From __start,
  // we JSR to main() which is at a known offset.
  //
  // For simplicity, let's use PC-relative BSR instead:
  // BSR.W has opcode 0x61 XX XX where XXXX is signed 16-bit displacement
  //
  // Displacement = target - (PC after fetching instruction)
  //              = (STARTUP_SIZE + mainOffsetInUserCode) - (STARTUP_MAIN_ADDR_OFFSET + 2)
  //              = STARTUP_SIZE + mainOffsetInUserCode - 8
  //
  // But BSR.W is only 4 bytes (opcode + displacement), not 6 like JSR abs.L
  // So we need to restructure the startup code. For now, let's keep JSR and
  // patch with offset from CODE 1 start.
  //
  // The absolute address needs to be computed at load time. For Classic Mac,
  // CODE segments are loaded into memory and their base address is known.
  // We'll use the offset from CODE 1 start as the "address" - the Segment Loader
  // will patch jump table entries but NOT inline JSR addresses.
  //
  // Better approach: Use JSR through A5 jump table instead, or use BSR.
  // Let's convert to BSR.L (32-bit displacement) which is 6 bytes total:
  // 0x61FF DDDD DDDD
  //
  // For now, let's just patch the offset and hope it works (small apps).
  // In CODE 1: header (4) + __start (0) + JSR at offset 4-9
  // main() at: header (4) + STARTUP_SIZE (18) + mainOffsetInUserCode
  //
  // If we're using JSR to an absolute address, we need the actual memory address.
  // But that's not known until runtime. So let's convert to BSR instead.

  // Patch BSR.W displacement (68000 compatible, 16-bit displacement)
  // BSR.W opcode: 0x6100 followed by 16-bit signed displacement
  // Displacement = target - (PC after reading opcode word)
  //              = target - (BSR_address + 2)
  //
  // In our code layout:
  //   Offset 0: LINK A6, #0 (4 bytes)
  //   Offset 4: BSR.W opcode 0x6100 (2 bytes)
  //   Offset 6: displacement (2 bytes)
  //   Offset 8: MOVE.W D0, -(SP)
  //   ...
  // main() is at offset STARTUP_SIZE (16) + mainOffsetInUserCode
  //
  // When executing BSR.W at offset 4:
  //   PC after reading opcode = 6
  //   Displacement = main_offset - 6 = (STARTUP_SIZE + mainOffsetInUserCode) - 6
  //                = 16 + mainOffsetInUserCode - 6
  //                = 10 + mainOffsetInUserCode

  uint32_t mainOffset = STARTUP_SIZE + mainOffsetInUserCode;
  int16_t displacement = static_cast<int16_t>(mainOffset - 6);

  // Verify displacement fits in 16 bits (±32KB range)
  if (mainOffset > 32767 + 6) {
    errorMsg = "main() is too far from startup code for BSR.W (>32KB)";
    return false;
  }

  // Patch bytes 6-7 with 16-bit displacement (big-endian)
  allCode[6] = (displacement >> 8) & 0xFF;
  allCode[7] = displacement & 0xFF;

  // Build jump table with __start as first entry (entry point)
  // __start is at offset 0 in CODE 1's code area (after 4-byte header)
  JumpTable jt;

  // Add __start as first jump table entry (the actual entry point)
  // Offset in CODE 1 = header size + 0 = 4
  jt.addEntry("__start", 1, CodeSegment::HEADER_SIZE);

  // Add other global functions to jump table (with adjusted offsets)
  // Their offsets are now: header + STARTUP_SIZE + original_offset
  for (const auto &[name, offset] : functions) {
    if (name != config->entrySymbol) {
      uint32_t adjustedOffset =
          CodeSegment::HEADER_SIZE + STARTUP_SIZE + offset;
      jt.addEntry(name, 1, adjustedOffset);
    }
  }

  // Create data section
  DataSection dataSection;
  dataSection.setData(allData);
  dataSection.setBSSSize(bssSize);

  // Set below A5 size in jump table
  jt.setBelowA5Size(dataSection.getBelowA5Size());

  // Create code segment
  CodeSegment codeSeg(1, config->segmentName);
  codeSeg.setCode(allCode);

  // Create SIZE resource
  SizeResource sizeRes;
  sizeRes.setFlags(config->sizeFlags);
  sizeRes.setPreferredSize(config->heapSize);
  sizeRes.setMinimumSize(config->minHeapSize);

  // Generate resource data
  auto code0Data = jt.generate();
  auto code1Data = codeSeg.generate(0, jt.size()); // All JT entries are for segment 1
  auto data0Data = dataSection.generate();
  auto sizeData = sizeRes.generate();

  // Create resource writer
  ResourceWriter rsrc;

  // Add CODE 0 (purgeable, protected)
  rsrc.addResource("CODE", 0, code0Data, resPurgeable | resProtected);

  // Add CODE 1 (purgeable, protected, with name)
  rsrc.addResource("CODE", 1, code1Data, resPurgeable | resProtected,
                   config->segmentName);

  // Add DATA 0 (purgeable, protected)
  if (!data0Data.empty()) {
    rsrc.addResource("DATA", 0, data0Data, resPurgeable | resProtected);
  }

  // Add SIZE -1 (no special attributes)
  rsrc.addResource("SIZE", -1, sizeData, 0);

  // Create empty data fork file
  std::error_code ec;
  {
    raw_fd_ostream dataFork(outputPath, ec, sys::fs::OF_None);
    if (ec) {
      errorMsg = "cannot create output file: " + outputPath;
      return false;
    }
    // Data fork is empty for classic 68K apps
  }

  // Write resource fork - try direct path first, fall back to .rsrc file
  bool rsrcWritten = rsrc.writeToFile(outputPath);

  // Also write a separate .rsrc file for use with ditto or AppleSingle
  std::string rsrcFilePath = outputPath + ".rsrc";
  rsrc.writeToRsrcFile(rsrcFilePath);

  if (!rsrcWritten) {
    // Resource fork writing failed, provide instructions
    if (config->verbose) {
      outs() << "Note: Could not write resource fork directly.\n";
      outs() << "Resource data written to: " << rsrcFilePath << "\n";
      outs() << "Use 'ditto' or AppleSingle to attach resource fork.\n";
    }
  }

  // Set file type to APPL and creator to the configured creator code
  // FinderInfo is 32 bytes: type (4) + creator (4) + flags (2) + location (4) +
  // folder (2) + icon ID (2) + unused (6) + script (1) + flags2 (1) + comment (2) + putaway (4)
  // We set type='APPL', creator from config, rest zeros
  std::string creator = config->creatorCode;
  if (creator.size() != 4)
    creator = "????";

  // Build FinderInfo hex string (32 bytes = 64 hex chars)
  char finderInfo[65];
  snprintf(finderInfo, sizeof(finderInfo),
           "4150504C"  // 'APPL' type
           "%02X%02X%02X%02X"  // creator code
           "0000000000000000"  // flags, location, folder
           "0000000000000000",  // rest of FinderInfo
           (uint8_t)creator[0], (uint8_t)creator[1],
           (uint8_t)creator[2], (uint8_t)creator[3]);

  // Try SetFile first (more reliable), fall back to xattr
  std::string setFileCmd = "/usr/bin/SetFile -t APPL -c '" + creator + "' \"" +
                           outputPath + "\" 2>/dev/null";
  if (system(setFileCmd.c_str()) != 0) {
    // SetFile failed, try xattr
    std::string xattrCmd = "xattr -wx com.apple.FinderInfo '" +
                           std::string(finderInfo) + "' \"" +
                           outputPath + "\" 2>/dev/null";
    system(xattrCmd.c_str());
  }

  if (config->verbose) {
    outs() << "Generated classic 68K application:\n";
    outs() << "  CODE 0: " << code0Data.size() << " bytes (jump table)\n";
    outs() << "  CODE 1: " << code1Data.size() << " bytes (code)\n";
    outs() << "    - Startup CRT: " << STARTUP_SIZE << " bytes\n";
    outs() << "    - User code: " << userCode.size() << " bytes\n";
    if (!data0Data.empty())
      outs() << "  DATA 0: " << data0Data.size() << " bytes (globals)\n";
    outs() << "  SIZE -1: " << sizeData.size() << " bytes\n";
    outs() << "  Entry point: __start -> " << config->entrySymbol << "\n";
  }

  return true;
}

} // namespace lld::classic68k
