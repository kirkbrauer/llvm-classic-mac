#!/bin/bash

# compile_minimal_test.sh
# Compiles minimal_test.c with both LLVM and Retro68 toolchains
# Outputs binaries to output/ and copies final versions to shared/

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "=========================================="
echo "Minimal Test Compilation Script"
echo "=========================================="

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
LLVM_READOBJ="${LLVM_BIN}/llvm-readobj"
# Use the real PEF InterfaceLib with LLVM
LLVM_INTERFACELIB="${REPO_ROOT}/build/lib/clang-runtimes/powerpc-apple-macos-9/lib/InterfaceLib"

# Retro68 toolchain paths (in parent directory)
RETRO68_BIN="${PARENT_ROOT}/Retro68-build/toolchain/bin"
RETRO68_GCC="${RETRO68_BIN}/powerpc-apple-macos-gcc"
RETRO68_LD="${RETRO68_BIN}/powerpc-apple-macos-ld"
RETRO68_OBJDUMP="${RETRO68_BIN}/powerpc-apple-macos-objdump"
RETRO68_MAKEPEF="${RETRO68_BIN}/MakePEF"
RETRO68_INTERFACELIB="${PARENT_ROOT}/Retro68-build/toolchain/powerpc-apple-macos/lib/libInterfaceLib.a"

# Check if test file exists
if [ ! -f "$TEST_FILE" ]; then
    echo -e "${RED}Error: Test file not found: $TEST_FILE${NC}"
    exit 1
fi

# Create output directories
echo "Creating output directories..."
mkdir -p "${OUTPUT_DIR}/llvm"
mkdir -p "${OUTPUT_DIR}/retro68"
mkdir -p "${OUTPUT_DIR}/comparison"

# Clean previous outputs
echo "Cleaning previous outputs..."
rm -f "${OUTPUT_DIR}/llvm/"*
rm -f "${OUTPUT_DIR}/retro68/"*
rm -f "${OUTPUT_DIR}/comparison/"*

# Clean shared directory test files
echo "Cleaning shared directory test files..."
rm -f "${SHARED_DIR}/MinimalTest_LLVM"
rm -f "${SHARED_DIR}/MinimalTest_Retro68"
rm -f "${SHARED_DIR}/MinimalTest_LLVM_Res"
rm -f "${SHARED_DIR}/MinimalTest_Retro68_Res"
rm -f "${SHARED_DIR}/MinimalTest_LLVM_Latest"

echo ""
echo "=========================================="
echo "Compiling with LLVM toolchain"
echo "=========================================="

# Step 1: Compile with LLVM clang
echo "Step 1: Compiling with clang..."
"${LLVM_CLANG}" \
    -target powerpc-apple-classic \
    -ffreestanding \
    -nostdlib \
    -nostdinc \
    -nostartfiles \
    -c "${TEST_FILE}" \
    -o "${OUTPUT_DIR}/llvm/minimal_test.o" \
    -v 2>&1 | tee "${OUTPUT_DIR}/llvm/compile_log.txt"

if [ $? -eq 0 ]; then
    echo -e "${GREEN}✓ LLVM compilation successful${NC}"
else
    echo -e "${RED}✗ LLVM compilation failed${NC}"
    exit 1
fi

# Step 2: Link with LLVM lld
echo "Step 2: Linking with lld..."
# Use standard library flag for linking
"${LLVM_LLD}" \
    -flavor pef \
    -e __start \
    -L "${REPO_ROOT}/build/lib/clang-runtimes/powerpc-apple-macos-9/lib" \
    "${OUTPUT_DIR}/llvm/minimal_test.o" \
    -lInterfaceLib \
    -o "${OUTPUT_DIR}/llvm/minimal_test.pef" \
    -v 2>&1 | tee "${OUTPUT_DIR}/llvm/link_log.txt"

# Check if the output file was created
if [ -f "${OUTPUT_DIR}/llvm/minimal_test.pef" ]; then
    echo -e "${GREEN}✓ LLVM linking successful${NC}"

    # Get file size
    LLVM_SIZE=$(ls -lh "${OUTPUT_DIR}/llvm/minimal_test.pef" | awk '{print $5}')
    echo "LLVM binary size: ${LLVM_SIZE}"
else
    echo -e "${RED}✗ LLVM linking failed - output file not created${NC}"

    # If standard approach failed, try direct file path
    echo "Retrying with direct library file..."
    "${LLVM_LLD}" \
        -flavor pef \
        -e __start \
        "${OUTPUT_DIR}/llvm/minimal_test.o" \
        "${LLVM_INTERFACELIB}" \
        -o "${OUTPUT_DIR}/llvm/minimal_test.pef" \
        -v 2>&1 | tee "${OUTPUT_DIR}/llvm/link_log_retry.txt"

    if [ -f "${OUTPUT_DIR}/llvm/minimal_test.pef" ]; then
        echo -e "${GREEN}✓ LLVM linking successful with direct library path${NC}"
        LLVM_SIZE=$(ls -lh "${OUTPUT_DIR}/llvm/minimal_test.pef" | awk '{print $5}')
        echo "LLVM binary size: ${LLVM_SIZE}"
    else
        echo -e "${RED}✗ LLVM linking failed${NC}"
        exit 1
    fi
fi

echo ""
echo "=========================================="
echo "Compiling with Retro68 toolchain"
echo "=========================================="

# Step 1: Compile with Retro68 GCC
echo "Step 1: Compiling with powerpc-apple-macos-gcc..."
"${RETRO68_GCC}" \
    -ffreestanding \
    -nostdlib \
    -nostdinc \
    -nostartfiles \
    -c "${TEST_FILE}" \
    -o "${OUTPUT_DIR}/retro68/minimal_test.o" \
    -v 2>&1 | tee "${OUTPUT_DIR}/retro68/compile_log.txt"

if [ $? -eq 0 ]; then
    echo -e "${GREEN}✓ Retro68 compilation successful${NC}"
else
    echo -e "${RED}✗ Retro68 compilation failed${NC}"
    exit 1
fi

# Step 2: Link to XCOFF with Retro68 ld
echo "Step 2: Linking to XCOFF..."
# Use text and data segment addresses to avoid overlap
"${RETRO68_LD}" \
    -e __start \
    -Ttext 0x1000 \
    -Tdata 0x2000 \
    "${OUTPUT_DIR}/retro68/minimal_test.o" \
    "${RETRO68_INTERFACELIB}" \
    -o "${OUTPUT_DIR}/retro68/minimal_test.xcoff" \
    2>&1 | tee "${OUTPUT_DIR}/retro68/link_log.txt"

if [ $? -eq 0 ]; then
    echo -e "${GREEN}✓ Retro68 XCOFF linking successful${NC}"
else
    echo -e "${RED}✗ Retro68 XCOFF linking failed${NC}"
    exit 1
fi

# Step 3: Convert XCOFF to PEF with MakePEF
echo "Step 3: Converting XCOFF to PEF with MakePEF..."
"${RETRO68_MAKEPEF}" \
    "${OUTPUT_DIR}/retro68/minimal_test.xcoff" \
    -o "${OUTPUT_DIR}/retro68/minimal_test.pef" \
    2>&1 | tee "${OUTPUT_DIR}/retro68/makepef_log.txt"

if [ $? -eq 0 ]; then
    echo -e "${GREEN}✓ Retro68 PEF conversion successful${NC}"

    # Get file size
    RETRO68_SIZE=$(ls -lh "${OUTPUT_DIR}/retro68/minimal_test.pef" | awk '{print $5}')
    echo "Retro68 binary size: ${RETRO68_SIZE}"
else
    echo -e "${RED}✗ Retro68 PEF conversion failed${NC}"
    exit 1
fi

echo ""
echo "=========================================="
echo "Copying binaries to shared directory"
echo "=========================================="

# Copy binaries to shared directory for emulator testing
echo "Copying LLVM binary to shared/MinimalTest_LLVM..."
cp "${OUTPUT_DIR}/llvm/minimal_test.pef" "${SHARED_DIR}/MinimalTest_LLVM"

echo "Copying Retro68 binary to shared/MinimalTest_Retro68..."
cp "${OUTPUT_DIR}/retro68/minimal_test.pef" "${SHARED_DIR}/MinimalTest_Retro68"

# Also copy as Latest for quick testing
cp "${OUTPUT_DIR}/llvm/minimal_test.pef" "${SHARED_DIR}/MinimalTest_LLVM_Latest"

echo ""
echo "=========================================="
echo "Quick PEF validation"
echo "=========================================="

# Check LLVM binary
echo "Checking LLVM binary..."
if xxd -l 4 "${OUTPUT_DIR}/llvm/minimal_test.pef" | grep -q "4a6f 7921"; then
    echo -e "${GREEN}✓ LLVM binary has valid PEF magic (Joy!)${NC}"
else
    echo -e "${RED}✗ LLVM binary missing PEF magic${NC}"
fi

# Check Retro68 binary
echo "Checking Retro68 binary..."
if xxd -l 4 "${OUTPUT_DIR}/retro68/minimal_test.pef" | grep -q "4a6f 7921"; then
    echo -e "${GREEN}✓ Retro68 binary has valid PEF magic (Joy!)${NC}"
else
    echo -e "${RED}✗ Retro68 binary missing PEF magic${NC}"
fi

echo ""
echo "=========================================="
echo "Compilation Summary"
echo "=========================================="
echo -e "${GREEN}✓ Both toolchains compiled successfully${NC}"
echo "LLVM binary:    ${LLVM_SIZE} -> ${SHARED_DIR}/MinimalTest_LLVM"
echo "Retro68 binary: ${RETRO68_SIZE} -> ${SHARED_DIR}/MinimalTest_Retro68"
echo ""
echo "Output files:"
echo "  ${OUTPUT_DIR}/llvm/minimal_test.o"
echo "  ${OUTPUT_DIR}/llvm/minimal_test.pef"
echo "  ${OUTPUT_DIR}/retro68/minimal_test.o"
echo "  ${OUTPUT_DIR}/retro68/minimal_test.xcoff"
echo "  ${OUTPUT_DIR}/retro68/minimal_test.pef"
echo ""
echo "Binaries ready for testing in Mac OS 9 emulator:"
echo "  ${SHARED_DIR}/MinimalTest_LLVM"
echo "  ${SHARED_DIR}/MinimalTest_Retro68"
echo ""
echo "Next steps:"
echo "  1. Run './scripts/disassemble_binaries.sh' to compare assembly"
echo "  2. Run './scripts/analyze_pef_structure.sh' to analyze PEF structure"
echo "  3. Test binaries in SheepShaver emulator"