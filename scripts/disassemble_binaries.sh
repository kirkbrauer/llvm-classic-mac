#!/bin/bash

# disassemble_binaries.sh
# Disassembles binaries from both LLVM and Retro68 toolchains
# Creates side-by-side comparison for analysis

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "=========================================="
echo "Binary Disassembly Script"
echo "=========================================="

# Setup paths - dynamically determine repo root from script location
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
PARENT_ROOT="$(cd "${REPO_ROOT}/.." && pwd)"

OUTPUT_DIR="${REPO_ROOT}/output"
COMPARISON_DIR="${OUTPUT_DIR}/comparison"

# LLVM toolchain paths
LLVM_BIN="${REPO_ROOT}/build/bin"
LLVM_OBJDUMP="${LLVM_BIN}/llvm-objdump"
LLVM_READOBJ="${LLVM_BIN}/llvm-readobj"

# Retro68 toolchain paths (in parent directory)
RETRO68_BIN="${PARENT_ROOT}/Retro68-build/toolchain/bin"
RETRO68_OBJDUMP="${RETRO68_BIN}/powerpc-apple-macos-objdump"
RETRO68_READELF="${RETRO68_BIN}/powerpc-apple-macos-readelf"

# Check if binaries exist
if [ ! -f "${OUTPUT_DIR}/llvm/minimal_test.pef" ]; then
    echo -e "${RED}Error: LLVM binary not found. Run compile_minimal_test.sh first${NC}"
    exit 1
fi

if [ ! -f "${OUTPUT_DIR}/retro68/minimal_test.pef" ]; then
    echo -e "${RED}Error: Retro68 binary not found. Run compile_minimal_test.sh first${NC}"
    exit 1
fi

# Create comparison directory
mkdir -p "${COMPARISON_DIR}"

echo ""
echo "=========================================="
echo "Disassembling LLVM binaries"
echo "=========================================="

# Disassemble LLVM object file
echo "Disassembling LLVM object file..."
"${LLVM_OBJDUMP}" -d -r --no-leading-addr --no-show-raw-insn \
    "${OUTPUT_DIR}/llvm/minimal_test.o" \
    > "${COMPARISON_DIR}/llvm_object.asm" 2>&1

echo "Created: ${COMPARISON_DIR}/llvm_object.asm"

# Disassemble LLVM PEF binary
echo "Disassembling LLVM PEF binary..."
"${LLVM_OBJDUMP}" -d -r --no-leading-addr --no-show-raw-insn \
    "${OUTPUT_DIR}/llvm/minimal_test.pef" \
    > "${COMPARISON_DIR}/llvm_pef.asm" 2>&1

echo "Created: ${COMPARISON_DIR}/llvm_pef.asm"

# Get detailed disassembly with relocations
echo "Getting detailed LLVM disassembly with relocations..."
"${LLVM_OBJDUMP}" -d -r \
    "${OUTPUT_DIR}/llvm/minimal_test.pef" \
    > "${COMPARISON_DIR}/llvm_pef_detailed.asm" 2>&1

echo "Created: ${COMPARISON_DIR}/llvm_pef_detailed.asm"

echo ""
echo "=========================================="
echo "Disassembling Retro68 binaries"
echo "=========================================="

# Disassemble Retro68 object file
echo "Disassembling Retro68 object file..."
"${RETRO68_OBJDUMP}" -d -r \
    "${OUTPUT_DIR}/retro68/minimal_test.o" \
    > "${COMPARISON_DIR}/retro68_object.asm" 2>&1

echo "Created: ${COMPARISON_DIR}/retro68_object.asm"

# Disassemble Retro68 XCOFF binary
echo "Disassembling Retro68 XCOFF binary..."
"${RETRO68_OBJDUMP}" -d -r \
    "${OUTPUT_DIR}/retro68/minimal_test.xcoff" \
    > "${COMPARISON_DIR}/retro68_xcoff.asm" 2>&1

echo "Created: ${COMPARISON_DIR}/retro68_xcoff.asm"

# Try to disassemble Retro68 PEF binary (might not work with standard objdump)
echo "Attempting to disassemble Retro68 PEF binary..."
"${RETRO68_OBJDUMP}" -d -r \
    "${OUTPUT_DIR}/retro68/minimal_test.pef" \
    > "${COMPARISON_DIR}/retro68_pef.asm" 2>&1 || {
    echo -e "${YELLOW}Note: Standard objdump may not fully support PEF format${NC}"
    # Try with LLVM objdump instead
    echo "Trying LLVM objdump for Retro68 PEF..."
    "${LLVM_OBJDUMP}" -d -r --no-leading-addr --no-show-raw-insn \
        "${OUTPUT_DIR}/retro68/minimal_test.pef" \
        > "${COMPARISON_DIR}/retro68_pef_llvm.asm" 2>&1
    echo "Created: ${COMPARISON_DIR}/retro68_pef_llvm.asm"
}

echo ""
echo "=========================================="
echo "Extracting key assembly patterns"
echo "=========================================="

# Extract __start function from both
echo "Extracting __start implementations..."

echo "=== LLVM __start ===" > "${COMPARISON_DIR}/start_comparison.txt"
grep -A 20 "__start:" "${COMPARISON_DIR}/llvm_pef.asm" >> "${COMPARISON_DIR}/start_comparison.txt" 2>/dev/null || \
    echo "Could not find __start in LLVM disassembly" >> "${COMPARISON_DIR}/start_comparison.txt"

echo "" >> "${COMPARISON_DIR}/start_comparison.txt"
echo "=== Retro68 __start ===" >> "${COMPARISON_DIR}/start_comparison.txt"
grep -A 20 "__start:" "${COMPARISON_DIR}/retro68_xcoff.asm" >> "${COMPARISON_DIR}/start_comparison.txt" 2>/dev/null || \
    echo "Could not find __start in Retro68 disassembly" >> "${COMPARISON_DIR}/start_comparison.txt"

echo "Created: ${COMPARISON_DIR}/start_comparison.txt"

# Look for ExitToShell calls
echo "Searching for ExitToShell references..."

echo "=== LLVM ExitToShell References ===" > "${COMPARISON_DIR}/exitoshell_refs.txt"
grep -i "ExitToShell" "${COMPARISON_DIR}/llvm_pef_detailed.asm" >> "${COMPARISON_DIR}/exitoshell_refs.txt" 2>/dev/null || \
    echo "No direct ExitToShell references found" >> "${COMPARISON_DIR}/exitoshell_refs.txt"

echo "" >> "${COMPARISON_DIR}/exitoshell_refs.txt"
echo "=== Retro68 ExitToShell References ===" >> "${COMPARISON_DIR}/exitoshell_refs.txt"
grep -i "ExitToShell" "${COMPARISON_DIR}/retro68_xcoff.asm" >> "${COMPARISON_DIR}/exitoshell_refs.txt" 2>/dev/null || \
    echo "No direct ExitToShell references found" >> "${COMPARISON_DIR}/exitoshell_refs.txt"

echo "Created: ${COMPARISON_DIR}/exitoshell_refs.txt"

echo ""
echo "=========================================="
echo "Analyzing calling conventions"
echo "=========================================="

# Create calling convention analysis
{
    echo "PowerPC Calling Convention Analysis"
    echo "===================================="
    echo ""
    echo "Expected PowerPC Classic Mac OS conventions:"
    echo "- r1: Stack pointer"
    echo "- r2: RTOC (Runtime Table of Contents)"
    echo "- r3-r10: Parameter passing"
    echo "- r3: Return value"
    echo "- r31: Frame pointer (if used)"
    echo ""
    echo "LLVM Assembly Analysis:"
    echo "-----------------------"

    # Check for stack operations in LLVM
    echo "Stack operations (r1):"
    grep -E "r1|sp" "${COMPARISON_DIR}/llvm_pef.asm" 2>/dev/null | head -5 || echo "None found"
    echo ""

    echo "RTOC operations (r2):"
    grep -E "r2|rtoc" "${COMPARISON_DIR}/llvm_pef.asm" 2>/dev/null | head -5 || echo "None found"
    echo ""

    echo "Retro68 Assembly Analysis:"
    echo "--------------------------"

    # Check for stack operations in Retro68
    echo "Stack operations (r1):"
    grep -E "r1|sp" "${COMPARISON_DIR}/retro68_xcoff.asm" 2>/dev/null | head -5 || echo "None found"
    echo ""

    echo "RTOC operations (r2):"
    grep -E "r2|rtoc" "${COMPARISON_DIR}/retro68_xcoff.asm" 2>/dev/null | head -5 || echo "None found"
    echo ""

} > "${COMPARISON_DIR}/calling_convention_analysis.txt"

echo "Created: ${COMPARISON_DIR}/calling_convention_analysis.txt"

echo ""
echo "=========================================="
echo "Creating side-by-side comparison"
echo "=========================================="

# Create a simple side-by-side view of object files
{
    echo "Side-by-Side Object File Comparison"
    echo "===================================="
    echo ""
    echo "LLVM Object                                    | Retro68 Object"
    echo "----------------------------------------------|----------------------------------------------"

    # Use paste to create side-by-side view (limited to first 50 lines)
    paste -d '|' \
        <(head -50 "${COMPARISON_DIR}/llvm_object.asm" | cut -c1-45) \
        <(head -50 "${COMPARISON_DIR}/retro68_object.asm" | cut -c1-45)

} > "${COMPARISON_DIR}/side_by_side.txt"

echo "Created: ${COMPARISON_DIR}/side_by_side.txt"

echo ""
echo "=========================================="
echo "Summary Statistics"
echo "=========================================="

# Count instructions in each binary
LLVM_INST_COUNT=$(grep -E "^\s+[a-z]+" "${COMPARISON_DIR}/llvm_pef.asm" 2>/dev/null | wc -l || echo "0")
RETRO68_INST_COUNT=$(grep -E "^\s+[a-z]+" "${COMPARISON_DIR}/retro68_xcoff.asm" 2>/dev/null | wc -l || echo "0")

{
    echo "Disassembly Statistics"
    echo "======================"
    echo ""
    echo "LLVM PEF:"
    echo "  Instruction count: ${LLVM_INST_COUNT}"
    echo "  File size: $(ls -lh "${OUTPUT_DIR}/llvm/minimal_test.pef" | awk '{print $5}')"
    echo ""
    echo "Retro68 PEF:"
    echo "  Instruction count: ${RETRO68_INST_COUNT}"
    echo "  File size: $(ls -lh "${OUTPUT_DIR}/retro68/minimal_test.pef" | awk '{print $5}')"
    echo ""
    echo "Difference:"
    echo "  Instruction count: $((RETRO68_INST_COUNT - LLVM_INST_COUNT))"
    echo ""
} > "${COMPARISON_DIR}/disasm_statistics.txt"

echo "Created: ${COMPARISON_DIR}/disasm_statistics.txt"

echo ""
echo "=========================================="
echo "Disassembly Complete"
echo "=========================================="
echo -e "${GREEN}✓ Disassembly completed successfully${NC}"
echo ""
echo "Generated files in ${COMPARISON_DIR}:"
echo "  - llvm_object.asm          : LLVM object file disassembly"
echo "  - llvm_pef.asm             : LLVM PEF binary disassembly"
echo "  - llvm_pef_detailed.asm    : LLVM PEF with full details"
echo "  - retro68_object.asm       : Retro68 object file disassembly"
echo "  - retro68_xcoff.asm        : Retro68 XCOFF disassembly"
echo "  - retro68_pef_llvm.asm     : Retro68 PEF (via LLVM objdump)"
echo "  - start_comparison.txt     : __start function comparison"
echo "  - exitoshell_refs.txt      : ExitToShell references"
echo "  - calling_convention_analysis.txt : Register usage analysis"
echo "  - side_by_side.txt         : Side-by-side view"
echo "  - disasm_statistics.txt    : Summary statistics"
echo ""
echo "Next step: Run './scripts/analyze_pef_structure.sh' to analyze PEF structure"