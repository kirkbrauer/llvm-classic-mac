#!/bin/bash

# quick_test.sh
# Quick compilation and testing script for rapid iteration
# Only compiles with LLVM and copies to shared for immediate testing

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

echo "=========================================="
echo -e "${CYAN}Quick Test - LLVM Toolchain${NC}"
echo "=========================================="

# Parse arguments
VERBOSE=false
COMPARE=false
DISASM=false

while [[ $# -gt 0 ]]; do
    case $1 in
        -v|--verbose)
            VERBOSE=true
            shift
            ;;
        -c|--compare)
            COMPARE=true
            shift
            ;;
        -d|--disasm)
            DISASM=true
            shift
            ;;
        -h|--help)
            echo "Usage: $0 [options]"
            echo "Options:"
            echo "  -v, --verbose   Show compilation output"
            echo "  -c, --compare   Also compile with Retro68 for comparison"
            echo "  -d, --disasm    Generate disassembly after compilation"
            echo "  -h, --help      Show this help message"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            echo "Use -h for help"
            exit 1
            ;;
    esac
done

# Setup paths - dynamically determine repo root from script location
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
PARENT_ROOT="$(cd "${REPO_ROOT}/.." && pwd)"

TEST_FILE="${REPO_ROOT}/macos-classic/test-programs/minimal_test.c"
OUTPUT_DIR="${REPO_ROOT}/output"
SHARED_DIR="${REPO_ROOT}/shared"

# LLVM toolchain paths
LLVM_BIN="${REPO_ROOT}/build/bin"
LLVM_CLANG="${LLVM_BIN}/clang"
LLVM_LLD="${LLVM_BIN}/ld.lld"
LLVM_OBJDUMP="${LLVM_BIN}/llvm-objdump"
LLVM_INTERFACELIB="${REPO_ROOT}/build/lib/clang-runtimes/powerpc-apple-macos-9/lib/InterfaceLib"

# Check if test file exists
if [ ! -f "$TEST_FILE" ]; then
    echo -e "${RED}Error: Test file not found: $TEST_FILE${NC}"
    exit 1
fi

# Create output directory
mkdir -p "${OUTPUT_DIR}/llvm"

# Timing
START_TIME=$(date +%s%N)

echo "Compiling ${TEST_FILE}..."

# Compile with LLVM
if $VERBOSE; then
    echo ""
    echo "Compilation command:"
    echo "${LLVM_CLANG} -target powerpc-apple-classic -ffreestanding -nostdlib -nostdinc -nostartfiles -c ${TEST_FILE} -o ${OUTPUT_DIR}/llvm/minimal_test.o"
    echo ""
fi

"${LLVM_CLANG}" \
    -target powerpc-apple-classic \
    -ffreestanding \
    -nostdlib \
    -nostdinc \
    -nostartfiles \
    -c "${TEST_FILE}" \
    -o "${OUTPUT_DIR}/llvm/minimal_test.o" \
    $(if $VERBOSE; then echo "-v"; fi) 2>&1 | \
    if $VERBOSE; then cat; else grep -E "error:|warning:" || true; fi

if [ ${PIPESTATUS[0]} -eq 0 ]; then
    echo -e "${GREEN}✓ Compilation successful${NC}"
else
    echo -e "${RED}✗ Compilation failed${NC}"
    exit 1
fi

# Link with LLVM
echo "Linking..."

if $VERBOSE; then
    echo ""
    echo "Link command:"
    echo "${LLVM_LLD} -flavor pef -e __start -L ${REPO_ROOT}/build/lib/clang-runtimes/powerpc-apple-macos-9/lib ${OUTPUT_DIR}/llvm/minimal_test.o -lInterfaceLib -o ${OUTPUT_DIR}/llvm/minimal_test.pef"
    echo ""
fi

"${LLVM_LLD}" \
    -flavor pef \
    -e __start \
    -L "${REPO_ROOT}/build/lib/clang-runtimes/powerpc-apple-macos-9/lib" \
    "${OUTPUT_DIR}/llvm/minimal_test.o" \
    -lInterfaceLib \
    -o "${OUTPUT_DIR}/llvm/minimal_test.pef" \
    $(if $VERBOSE; then echo "-v"; fi) 2>&1 | \
    if $VERBOSE; then cat; else grep -E "error:|warning:" || true; fi

if [ ${PIPESTATUS[0]} -eq 0 ]; then
    echo -e "${GREEN}✓ Linking successful${NC}"
else
    echo -e "${RED}✗ Linking failed${NC}"
    exit 1
fi

# Calculate compilation time
END_TIME=$(date +%s%N)
ELAPSED_NS=$((END_TIME - START_TIME))
ELAPSED_MS=$((ELAPSED_NS / 1000000))

echo "Compilation time: ${ELAPSED_MS}ms"

# Get file size
SIZE=$(ls -lh "${OUTPUT_DIR}/llvm/minimal_test.pef" | awk '{print $5}')
echo "Binary size: ${SIZE}"

# Quick PEF validation
if xxd -l 4 "${OUTPUT_DIR}/llvm/minimal_test.pef" | grep -q "4a6f 7921"; then
    echo -e "${GREEN}✓ Valid PEF magic (Joy!)${NC}"
else
    echo -e "${RED}✗ Invalid PEF magic${NC}"
    echo "First 16 bytes:"
    xxd -l 16 "${OUTPUT_DIR}/llvm/minimal_test.pef"
fi

# Copy to shared directory
echo ""
echo "Copying to shared directory..."
cp "${OUTPUT_DIR}/llvm/minimal_test.pef" "${SHARED_DIR}/MinimalTest_LLVM_Latest"
echo -e "${GREEN}✓ Copied to: ${SHARED_DIR}/MinimalTest_LLVM_Latest${NC}"

# Optional: Generate disassembly
if $DISASM; then
    echo ""
    echo "Generating disassembly..."
    "${LLVM_OBJDUMP}" -d --no-leading-addr --no-show-raw-insn \
        "${OUTPUT_DIR}/llvm/minimal_test.pef" \
        > "${OUTPUT_DIR}/llvm/minimal_test_quick.asm" 2>&1

    echo "Disassembly saved to: ${OUTPUT_DIR}/llvm/minimal_test_quick.asm"

    # Show __start function
    echo ""
    echo "__start function:"
    echo "-----------------"
    grep -A 10 "__start:" "${OUTPUT_DIR}/llvm/minimal_test_quick.asm" 2>/dev/null || \
        echo "Could not find __start in disassembly"
fi

# Optional: Compare with Retro68
if $COMPARE; then
    echo ""
    echo "=========================================="
    echo "Compiling with Retro68 for comparison"
    echo "=========================================="

    # Retro68 paths (in parent directory)
    RETRO68_BIN="${PARENT_ROOT}/Retro68-build/toolchain/bin"
    RETRO68_GCC="${RETRO68_BIN}/powerpc-apple-macos-gcc"
    RETRO68_LD="${RETRO68_BIN}/powerpc-apple-macos-ld"
    RETRO68_MAKEPEF="${RETRO68_BIN}/MakePEF"
    RETRO68_INTERFACELIB="${PARENT_ROOT}/Retro68-build/toolchain/powerpc-apple-macos/lib/libInterfaceLib.a"

    mkdir -p "${OUTPUT_DIR}/retro68"

    # Compile
    "${RETRO68_GCC}" \
        -ffreestanding -nostdlib -nostdinc -nostartfiles \
        -c "${TEST_FILE}" \
        -o "${OUTPUT_DIR}/retro68/minimal_test.o" 2>&1 | \
        grep -E "error:|warning:" || true

    # Link
    "${RETRO68_LD}" \
        -e __start \
        "${OUTPUT_DIR}/retro68/minimal_test.o" \
        "${RETRO68_INTERFACELIB}" \
        -o "${OUTPUT_DIR}/retro68/minimal_test.xcoff" 2>&1 | \
        grep -E "error:|warning:" || true

    # Convert to PEF
    "${RETRO68_MAKEPEF}" \
        "${OUTPUT_DIR}/retro68/minimal_test.xcoff" \
        -o "${OUTPUT_DIR}/retro68/minimal_test.pef" 2>&1 | \
        grep -E "error:|warning:" || true

    # Copy to shared
    cp "${OUTPUT_DIR}/retro68/minimal_test.pef" "${SHARED_DIR}/MinimalTest_Retro68_Latest"

    # Compare sizes
    RETRO68_SIZE=$(ls -lh "${OUTPUT_DIR}/retro68/minimal_test.pef" | awk '{print $5}')
    echo ""
    echo "Size comparison:"
    echo "  LLVM:    ${SIZE}"
    echo "  Retro68: ${RETRO68_SIZE}"
fi

echo ""
echo "=========================================="
echo -e "${GREEN}Quick test complete!${NC}"
echo "=========================================="
echo ""
echo "Binary ready for testing:"
echo "  ${SHARED_DIR}/MinimalTest_LLVM_Latest"

if $COMPARE; then
    echo "  ${SHARED_DIR}/MinimalTest_Retro68_Latest"
fi

echo ""
echo "To test in Mac OS 9:"
echo "  1. Start emulator"
echo "  2. Double-click MinimalTest_LLVM_Latest in shared folder"
echo ""
echo "For full test suite, run:"
echo "  ${REPO_ROOT}/scripts/run_full_test.sh"

exit 0