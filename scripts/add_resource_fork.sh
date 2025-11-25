#!/bin/bash

# add_resource_fork.sh
# Adds resource fork to PEF binaries using Rez tool
# Creates versions with resources for both LLVM and Retro68 binaries

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "=========================================="
echo "Resource Fork Addition Script"
echo "=========================================="

# Setup paths - dynamically determine repo root from script location
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
PARENT_ROOT="$(cd "${REPO_ROOT}/.." && pwd)"

OUTPUT_DIR="${REPO_ROOT}/output"
SHARED_DIR="${REPO_ROOT}/shared"
RESOURCE_DIR="${REPO_ROOT}/macos-classic/resources"
RESOURCE_FILE="${RESOURCE_DIR}/minimal_app.r"

# Retro68 Rez tool (in parent directory)
REZ_TOOL="${PARENT_ROOT}/Retro68-build/toolchain/bin/Rez"

# Check if Rez tool exists
if [ ! -f "$REZ_TOOL" ]; then
    echo -e "${RED}Error: Rez tool not found at $REZ_TOOL${NC}"
    echo "Make sure Retro68 is built correctly"
    exit 1
fi

# Check if resource file exists
if [ ! -f "$RESOURCE_FILE" ]; then
    echo -e "${RED}Error: Resource file not found at $RESOURCE_FILE${NC}"
    exit 1
fi

# Check if binaries exist
if [ ! -f "${OUTPUT_DIR}/llvm/minimal_test.pef" ]; then
    echo -e "${RED}Error: LLVM binary not found. Run compile_minimal_test.sh first${NC}"
    exit 1
fi

if [ ! -f "${OUTPUT_DIR}/retro68/minimal_test.pef" ]; then
    echo -e "${RED}Error: Retro68 binary not found. Run compile_minimal_test.sh first${NC}"
    exit 1
fi

echo ""
echo "=========================================="
echo "Adding resources to LLVM binary"
echo "=========================================="

# Create a copy of the LLVM binary for resource addition
echo "Creating copy of LLVM binary..."
cp "${OUTPUT_DIR}/llvm/minimal_test.pef" "${OUTPUT_DIR}/llvm/minimal_test_res.pef"

# Add resources to LLVM binary
echo "Adding resources with Rez..."
# Include paths for resource headers
"${REZ_TOOL}" \
    -I "${PROJECT_ROOT}/Retro68-build/toolchain/m68k-apple-macos/RIncludes" \
    -I "${PROJECT_ROOT}/Retro68-build/toolchain/powerpc-apple-macos/RIncludes" \
    -o "${OUTPUT_DIR}/llvm/minimal_test_res.pef" \
    -a \
    "${RESOURCE_FILE}" 2>&1 | tee "${OUTPUT_DIR}/llvm/rez_log.txt"

if [ $? -eq 0 ]; then
    echo -e "${GREEN}✓ Resources added to LLVM binary${NC}"

    # Check file size difference
    ORIG_SIZE=$(ls -l "${OUTPUT_DIR}/llvm/minimal_test.pef" | awk '{print $5}')
    RES_SIZE=$(ls -l "${OUTPUT_DIR}/llvm/minimal_test_res.pef" | awk '{print $5}')
    SIZE_DIFF=$((RES_SIZE - ORIG_SIZE))

    echo "Original size: ${ORIG_SIZE} bytes"
    echo "With resources: ${RES_SIZE} bytes"
    echo "Resource fork added: ${SIZE_DIFF} bytes"
else
    echo -e "${RED}✗ Failed to add resources to LLVM binary${NC}"
    exit 1
fi

# Copy to shared directory
echo "Copying to shared directory..."
cp "${OUTPUT_DIR}/llvm/minimal_test_res.pef" "${SHARED_DIR}/MinimalTest_LLVM_Res"
echo "Created: ${SHARED_DIR}/MinimalTest_LLVM_Res"

echo ""
echo "=========================================="
echo "Adding resources to Retro68 binary"
echo "=========================================="

# Create a copy of the Retro68 binary for resource addition
echo "Creating copy of Retro68 binary..."
cp "${OUTPUT_DIR}/retro68/minimal_test.pef" "${OUTPUT_DIR}/retro68/minimal_test_res.pef"

# Add resources to Retro68 binary
echo "Adding resources with Rez..."
# Include paths for resource headers
"${REZ_TOOL}" \
    -I "${PROJECT_ROOT}/Retro68-build/toolchain/m68k-apple-macos/RIncludes" \
    -I "${PROJECT_ROOT}/Retro68-build/toolchain/powerpc-apple-macos/RIncludes" \
    -o "${OUTPUT_DIR}/retro68/minimal_test_res.pef" \
    -a \
    "${RESOURCE_FILE}" 2>&1 | tee "${OUTPUT_DIR}/retro68/rez_log.txt"

if [ $? -eq 0 ]; then
    echo -e "${GREEN}✓ Resources added to Retro68 binary${NC}"

    # Check file size difference
    ORIG_SIZE=$(ls -l "${OUTPUT_DIR}/retro68/minimal_test.pef" | awk '{print $5}')
    RES_SIZE=$(ls -l "${OUTPUT_DIR}/retro68/minimal_test_res.pef" | awk '{print $5}')
    SIZE_DIFF=$((RES_SIZE - ORIG_SIZE))

    echo "Original size: ${ORIG_SIZE} bytes"
    echo "With resources: ${RES_SIZE} bytes"
    echo "Resource fork added: ${SIZE_DIFF} bytes"
else
    echo -e "${RED}✗ Failed to add resources to Retro68 binary${NC}"
    exit 1
fi

# Copy to shared directory
echo "Copying to shared directory..."
cp "${OUTPUT_DIR}/retro68/minimal_test_res.pef" "${SHARED_DIR}/MinimalTest_Retro68_Res"
echo "Created: ${SHARED_DIR}/MinimalTest_Retro68_Res"

echo ""
echo "=========================================="
echo "Verifying resource forks"
echo "=========================================="

# Use DeRez to verify resources were added correctly
if command -v DeRez &> /dev/null; then
    echo "Using DeRez to verify resources..."

    echo "LLVM binary resources:"
    DeRez "${OUTPUT_DIR}/llvm/minimal_test_res.pef" 2>/dev/null | head -20 || \
        echo "  (DeRez not available on this system)"

    echo ""
    echo "Retro68 binary resources:"
    DeRez "${OUTPUT_DIR}/retro68/minimal_test_res.pef" 2>/dev/null | head -20 || \
        echo "  (DeRez not available on this system)"
else
    echo "DeRez not available - skipping resource verification"
    echo "Resources should be visible when running in Mac OS 9"
fi

echo ""
echo "=========================================="
echo "Setting file type and creator codes"
echo "=========================================="

# Function to set file type and creator using multiple methods
set_file_type_creator() {
    local file=$1
    local type="APPL"
    local creator="????"

    echo "Setting type/creator for $(basename $file)..."

    # Method 1: Try SetFile if available
    if command -v SetFile &> /dev/null; then
        if SetFile -t "$type" -c "$creator" "$file" 2>/dev/null; then
            echo "  ✓ SetFile succeeded"
            return 0
        fi
    fi

    # Method 2: Try xattr with FinderInfo
    # APPL???? in hex: 4150504C3F3F3F3F followed by 24 zero bytes
    if command -v xattr &> /dev/null; then
        if xattr -wx com.apple.FinderInfo \
            "4150504C3F3F3F3F00000000000000000000000000000000000000000000000000000000" \
            "$file" 2>/dev/null; then
            echo "  ✓ xattr succeeded"
            return 0
        fi
    fi

    echo "  ⚠ Could not set file type/creator"
    return 1
}

# Set type and creator for both binaries
set_file_type_creator "${SHARED_DIR}/MinimalTest_LLVM_Res"
set_file_type_creator "${SHARED_DIR}/MinimalTest_Retro68_Res"

# Also set for non-resource versions for testing
set_file_type_creator "${SHARED_DIR}/MinimalTest_LLVM"
set_file_type_creator "${SHARED_DIR}/MinimalTest_Retro68"

echo -e "${GREEN}✓ File type/creator setting complete${NC}"

echo ""
echo "=========================================="
echo "Resource Fork Summary"
echo "=========================================="
echo -e "${GREEN}✓ Resource forks added successfully${NC}"
echo ""
echo "Files with resources in output directory:"
echo "  ${OUTPUT_DIR}/llvm/minimal_test_res.pef"
echo "  ${OUTPUT_DIR}/retro68/minimal_test_res.pef"
echo ""
echo "Files ready for testing in Mac OS 9 (shared directory):"
echo "  ${SHARED_DIR}/MinimalTest_LLVM_Res"
echo "  ${SHARED_DIR}/MinimalTest_Retro68_Res"
echo ""
echo "Resources added:"
echo "  - SIZE resource (memory requirements)"
echo "  - vers resource (version 1.0)"
echo "  - STR resource (application name)"
echo ""
echo "Next steps:"
echo "  1. Test all four binaries in SheepShaver:"
echo "     - MinimalTest_LLVM (no resources)"
echo "     - MinimalTest_LLVM_Res (with resources)"
echo "     - MinimalTest_Retro68 (no resources)"
echo "     - MinimalTest_Retro68_Res (with resources)"
echo "  2. Check if resources appear in Get Info dialog"
echo "  3. Verify clean exit with ExitToShell"