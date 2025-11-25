#!/bin/bash
# Build and prepare Mac OS 9 C++ programs with runtime support
# Usage: ./build_cxx_with_runtime.sh <test_name> [reference_binary]
#   test_name: Name without .cpp extension (e.g., beep_cxx_atexit)
#   reference_binary: Optional CodeWarrior binary for resource fork (defaults to beep_test_codewarrior)

set -e  # Exit on error

if [ -z "$1" ]; then
    echo "Usage: $0 <test_name> [reference_binary]"
    echo "Example: $0 beep_cxx_atexit"
    echo "Example: $0 beep_cxx_simple beep_test_codewarrior"
    exit 1
fi

TEST_NAME="$1"
REFERENCE_BINARY="${2:-beep_test_codewarrior}"

# Paths - dynamically determine repo root from script location
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

CLANGXX="${REPO_ROOT}/build/bin/clang++"
LLD="${REPO_ROOT}/build/bin/ld.lld"
TEST_DIR="${REPO_ROOT}/macos-classic/test-programs"
SHARED_DIR="${REPO_ROOT}/shared"
RESOURCES_DIR="${REPO_ROOT}/macos-classic/resources"
RUNTIME_DIR="${REPO_ROOT}/build/lib/clang/20/lib/macosclassic"

SOURCE_FILE="${TEST_DIR}/${TEST_NAME}.cpp"
OBJECT_FILE="${TEST_DIR}/${TEST_NAME}.o"
PEF_FILE="${TEST_DIR}/${TEST_NAME}.pef"
OUTPUT_FILE="${SHARED_DIR}/${TEST_NAME}"
REFERENCE_FILE="${RESOURCES_DIR}/${REFERENCE_BINARY}"

# Check if source file exists
if [ ! -f "$SOURCE_FILE" ]; then
    echo "Error: Source file not found: $SOURCE_FILE"
    exit 1
fi

echo "Building ${TEST_NAME} with C++ runtime..."
echo "========================================"
echo ""

# Step 1: Compile and link in one step
# The toolchain automatically:
# - Adds -e __start (default entry point)
# - Links macos_classic_start.o (provides __start → main wrapper)
# - Links macos_classic_cxx.o (provides atexit, exit, __cxa_atexit, __cxa_finalize)
# - Links macos_classic_qd.o (provides QuickDraw globals)
# - Disables exceptions and RTTI (not supported yet)
echo "1. Compiling and linking ${TEST_NAME}.cpp..."
cd "$TEST_DIR"
"$CLANGXX" --target=powerpc-apple-classic \
    -fno-exceptions -fno-rtti \
    "${TEST_NAME}.cpp" \
    -lInterfaceLib \
    -o "${TEST_NAME}.pef"
echo "   ✓ Built ${TEST_NAME}.pef"
echo "   Runtime files automatically linked by toolchain"
echo ""

# Step 2: Copy to shared directory
echo "2. Preparing for Mac OS 9..."
mkdir -p "$SHARED_DIR"
cp "${TEST_NAME}.pef" "$OUTPUT_FILE"
echo "   ✓ Copied to shared/${TEST_NAME}"
echo ""

# Step 3: Apply resource fork
if [ -f "$REFERENCE_FILE" ]; then
    echo "3. Applying resource fork from ${REFERENCE_BINARY}..."
    cp "${REFERENCE_FILE}/..namedfork/rsrc" "${OUTPUT_FILE}/..namedfork/rsrc"
    echo "   ✓ Resource fork applied"
else
    echo "3. Warning: Reference binary not found: $REFERENCE_FILE"
    echo "   Skipping resource fork (binary may not run on Mac OS 9)"
fi
echo ""

# Step 4: Set file type and creator
echo "4. Setting file type and creator..."
SetFile -t APPL -c '????' "$OUTPUT_FILE"
echo "   ✓ File attributes set (APPL/????)"
echo ""

echo "========================================"
echo "✓ Build complete!"
echo ""
echo "Output:   ${OUTPUT_FILE}"
echo "Size:     $(ls -lh "${OUTPUT_FILE}" | awk '{print $5}')"
echo ""
echo "C++ Features Supported:"
echo "  • Classes with constructors/destructors"
echo "  • Standard atexit() for cleanup handlers"
echo "  • __cxa_atexit/__cxa_finalize for C++ destructors"
echo "  • Placement new for manual object construction"
echo "  • Mac OS headers via extern \"C\""
echo ""
echo "Not Yet Supported:"
echo "  • Exceptions (-fno-exceptions required)"
echo "  • RTTI (-fno-rtti required)"
echo "  • Global C++ objects (no .ctors section yet)"
echo ""
echo "Ready to test on Mac OS 9!"
