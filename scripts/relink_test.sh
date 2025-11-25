#!/bin/bash
# Relink an existing object file and prepare for Mac OS 9
# Usage: ./relink_test.sh <test_name> [reference_binary]
#   test_name: Name without .o extension (e.g., minimal_test, beep_simple, beep_test)
#   reference_binary: Optional CodeWarrior binary for resource fork (defaults to test_name_codewarrior)

set -e  # Exit on error

if [ -z "$1" ]; then
    echo "Usage: $0 <test_name> [reference_binary]"
    echo "Example: $0 minimal_test"
    echo "Example: $0 beep_simple beep_test_codewarrior"
    exit 1
fi

TEST_NAME="$1"
REFERENCE_BINARY="${2:-${TEST_NAME}_codewarrior}"

# Paths - dynamically determine repo root from script location
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

LLD="${REPO_ROOT}/build/bin/ld.lld"
TEST_DIR="${REPO_ROOT}/macos-classic/test-programs"
SHARED_DIR="${REPO_ROOT}/shared"
RESOURCES_DIR="${REPO_ROOT}/macos-classic/resources"

OBJECT_FILE="${TEST_DIR}/${TEST_NAME}.o"
PEF_FILE="${TEST_DIR}/${TEST_NAME}_llvm.pef"
OUTPUT_FILE="${SHARED_DIR}/${TEST_NAME}_llvm"
REFERENCE_FILE="${RESOURCES_DIR}/${REFERENCE_BINARY}"

# Check if object file exists
if [ ! -f "$OBJECT_FILE" ]; then
    echo "Error: Object file not found: $OBJECT_FILE"
    echo "Run build_test.sh first to compile the source."
    exit 1
fi

echo "Relinking ${TEST_NAME}..."
echo "----------------------------------------"

# Step 1: Link
echo "1. Linking ${TEST_NAME}.o..."
cd "$TEST_DIR"
"$LLD" -flavor pef -e __start "${TEST_NAME}.o" -lInterfaceLib -o "${TEST_NAME}_llvm.pef"
echo "   ✓ Linked to ${TEST_NAME}_llvm.pef"

# Step 2: Copy to shared directory
echo "2. Preparing for Mac OS 9..."
mkdir -p "$SHARED_DIR"
cp "${TEST_NAME}_llvm.pef" "$OUTPUT_FILE"
echo "   ✓ Copied to shared/${TEST_NAME}_llvm"

# Step 3: Apply resource fork
if [ -f "$REFERENCE_FILE" ]; then
    echo "3. Applying resource fork from ${REFERENCE_BINARY}..."
    cp "${REFERENCE_FILE}/..namedfork/rsrc" "${OUTPUT_FILE}/..namedfork/rsrc"
    echo "   ✓ Resource fork applied"
else
    echo "3. Warning: Reference binary not found: $REFERENCE_FILE"
    echo "   Skipping resource fork (binary may not run on Mac OS 9)"
fi

# Step 4: Set file type and creator
echo "4. Setting file type and creator..."
SetFile -t APPL -c '????' "$OUTPUT_FILE"
echo "   ✓ File attributes set (APPL/????)"

echo "----------------------------------------"
echo "✓ Relink complete!"
echo ""
echo "Output: ${OUTPUT_FILE}"
echo "Size:   $(ls -lh "$OUTPUT_FILE" | awk '{print $5}')"
echo ""
echo "Ready to test on Mac OS 9!"
