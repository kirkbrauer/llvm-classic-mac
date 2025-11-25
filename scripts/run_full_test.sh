#!/bin/bash

# run_full_test.sh
# Master test runner that orchestrates all testing scripts
# Compiles, analyzes, and prepares binaries for Mac OS 9 testing

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
MAGENTA='\033[0;35m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# Timing
START_TIME=$(date +%s)

echo "=========================================="
echo -e "${CYAN}Mac OS 9 Toolchain Full Test Suite${NC}"
echo "=========================================="
echo "Started: $(date)"
echo ""

# Setup paths - dynamically determine repo root from script location
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

SCRIPTS_DIR="${REPO_ROOT}/scripts"
OUTPUT_DIR="${REPO_ROOT}/output"
SHARED_DIR="${REPO_ROOT}/shared"
DOCS_DIR="${REPO_ROOT}/macos-classic/docs"

# Check if test file exists
if [ ! -f "${REPO_ROOT}/macos-classic/test-programs/minimal_test.c" ]; then
    echo -e "${RED}Error: minimal_test.c not found${NC}"
    exit 1
fi

# Function to run a step
run_step() {
    local step_name=$1
    local script_path=$2
    local step_num=$3
    local total_steps=$4

    echo ""
    echo -e "${BLUE}[$step_num/$total_steps]${NC} ${MAGENTA}$step_name${NC}"
    echo "----------------------------------------"

    if [ -f "$script_path" ]; then
        if "$script_path"; then
            echo -e "${GREEN}✓ $step_name completed successfully${NC}"
            return 0
        else
            echo -e "${RED}✗ $step_name failed${NC}"
            return 1
        fi
    else
        echo -e "${RED}✗ Script not found: $script_path${NC}"
        return 1
    fi
}

# Total number of steps
TOTAL_STEPS=5

echo -e "${CYAN}Running full test suite with $TOTAL_STEPS steps...${NC}"

# Step 1: Compile with both toolchains
if ! run_step "Compiling with both toolchains" \
    "${SCRIPTS_DIR}/compile_minimal_test.sh" 1 $TOTAL_STEPS; then
    echo -e "${RED}Compilation failed. Stopping test suite.${NC}"
    exit 1
fi

# Step 2: Disassemble binaries
if ! run_step "Disassembling binaries" \
    "${SCRIPTS_DIR}/disassemble_binaries.sh" 2 $TOTAL_STEPS; then
    echo -e "${YELLOW}Warning: Disassembly had issues but continuing...${NC}"
fi

# Step 3: Analyze PEF structure
if ! run_step "Analyzing PEF structure" \
    "${SCRIPTS_DIR}/analyze_pef_structure.sh" 3 $TOTAL_STEPS; then
    echo -e "${YELLOW}Warning: PEF analysis had issues but continuing...${NC}"
fi

# Step 4: Add resource forks
if ! run_step "Adding resource forks" \
    "${SCRIPTS_DIR}/add_resource_fork.sh" 4 $TOTAL_STEPS; then
    echo -e "${YELLOW}Warning: Resource fork addition had issues but continuing...${NC}"
fi

# Step 5: Validate calling conventions
if ! run_step "Validating calling conventions" \
    "${SCRIPTS_DIR}/validate_calling_convention.sh" 5 $TOTAL_STEPS; then
    echo -e "${YELLOW}Warning: Convention validation had issues but continuing...${NC}"
fi

echo ""
echo "=========================================="
echo -e "${CYAN}Test Results Summary${NC}"
echo "=========================================="

# Check what files were created
echo ""
echo "Binary Files Created:"
echo "---------------------"

check_file() {
    local file=$1
    local desc=$2
    if [ -f "$file" ]; then
        size=$(ls -lh "$file" | awk '{print $5}')
        echo -e "${GREEN}✓${NC} $desc ($size)"
        return 0
    else
        echo -e "${RED}✗${NC} $desc (not found)"
        return 1
    fi
}

# Check output binaries
check_file "${OUTPUT_DIR}/llvm/minimal_test.pef" "LLVM PEF binary"
check_file "${OUTPUT_DIR}/retro68/minimal_test.pef" "Retro68 PEF binary"
check_file "${OUTPUT_DIR}/llvm/minimal_test_res.pef" "LLVM PEF with resources"
check_file "${OUTPUT_DIR}/retro68/minimal_test_res.pef" "Retro68 PEF with resources"

echo ""
echo "Shared Directory Files (for emulator):"
echo "---------------------------------------"

check_file "${SHARED_DIR}/MinimalTest_LLVM" "LLVM binary"
check_file "${SHARED_DIR}/MinimalTest_Retro68" "Retro68 binary"
check_file "${SHARED_DIR}/MinimalTest_LLVM_Res" "LLVM with resources"
check_file "${SHARED_DIR}/MinimalTest_Retro68_Res" "Retro68 with resources"

echo ""
echo "Analysis Reports:"
echo "-----------------"

# Count analysis files
analysis_count=$(ls -1 "${OUTPUT_DIR}/comparison/"*.txt 2>/dev/null | wc -l || echo 0)
echo "Generated ${analysis_count} analysis files in ${OUTPUT_DIR}/comparison/"

# Show key validation results if available
if [ -f "${OUTPUT_DIR}/comparison/pef_validation_checklist.txt" ]; then
    echo ""
    echo "PEF Validation Results:"
    grep "✓\|✗" "${OUTPUT_DIR}/comparison/pef_validation_checklist.txt" | head -10
fi

if [ -f "${OUTPUT_DIR}/comparison/convention_validation_summary.txt" ]; then
    echo ""
    echo "Calling Convention Results:"
    grep "Overall:" "${OUTPUT_DIR}/comparison/convention_validation_summary.txt"
fi

echo ""
echo "=========================================="
echo -e "${CYAN}Quick Comparison${NC}"
echo "=========================================="

# Binary size comparison
if [ -f "${OUTPUT_DIR}/llvm/minimal_test.pef" ] && [ -f "${OUTPUT_DIR}/retro68/minimal_test.pef" ]; then
    llvm_size=$(stat -f%z "${OUTPUT_DIR}/llvm/minimal_test.pef" 2>/dev/null || stat -c%s "${OUTPUT_DIR}/llvm/minimal_test.pef" 2>/dev/null)
    retro68_size=$(stat -f%z "${OUTPUT_DIR}/retro68/minimal_test.pef" 2>/dev/null || stat -c%s "${OUTPUT_DIR}/retro68/minimal_test.pef" 2>/dev/null)

    size_diff=$((retro68_size - llvm_size))
    if [ $llvm_size -gt 0 ]; then
        percent_diff=$((size_diff * 100 / llvm_size))
    else
        percent_diff=0
    fi

    echo "Binary Size Comparison:"
    echo "  LLVM:    ${llvm_size} bytes"
    echo "  Retro68: ${retro68_size} bytes"
    echo "  Difference: ${size_diff} bytes (Retro68 is ${percent_diff}% larger)"
fi

# PEF magic verification
echo ""
echo "PEF Magic Verification:"
for binary in "${OUTPUT_DIR}/llvm/minimal_test.pef" "${OUTPUT_DIR}/retro68/minimal_test.pef"; do
    if [ -f "$binary" ]; then
        name=$(basename $(dirname "$binary"))
        if xxd -l 4 "$binary" 2>/dev/null | grep -q "4a6f 7921"; then
            echo -e "  ${GREEN}✓${NC} $name: Valid PEF magic (Joy!)"
        else
            echo -e "  ${RED}✗${NC} $name: Invalid PEF magic"
        fi
    fi
done

echo ""
echo "=========================================="
echo -e "${CYAN}Creating Test Checklist${NC}"
echo "=========================================="

# Create test checklist for Mac OS 9
cat > "${SHARED_DIR}/TEST_CHECKLIST.txt" << 'EOF'
Mac OS 9 Test Checklist
========================
Generated: $(date)

Test Instructions:
1. Open the shared folder in Mac OS 9
2. Double-click each application below
3. Record the results

Expected behavior: Each app should exit cleanly with no error dialog

Applications to Test:
---------------------

[ ] MinimalTest_LLVM
    Expected: Clean exit
    Actual: ________________________
    Notes: _________________________

[ ] MinimalTest_Retro68
    Expected: Clean exit
    Actual: ________________________
    Notes: _________________________

[ ] MinimalTest_LLVM_Res
    Expected: Clean exit, version in Get Info
    Actual: ________________________
    Notes: _________________________

[ ] MinimalTest_Retro68_Res
    Expected: Clean exit, version in Get Info
    Actual: ________________________
    Notes: _________________________

Debugging (if crashes occur):
------------------------------
1. Open MacsBug (Command-Power)
2. Type 'log' to start logging
3. Double-click the crashing app
4. In MacsBug, type:
   - 'sc' for stack crawl
   - 'il' to show around PC
   - 'td' for register dump
5. Type 'g' to continue
6. Save the log output

Results Summary:
----------------
LLVM binaries working:     YES / NO
Retro68 binaries working:  YES / NO
Resources visible:         YES / NO
Any crashes:              YES / NO

Additional Notes:
_________________________________
_________________________________
_________________________________
EOF

echo -e "${GREEN}✓ Created test checklist: ${SHARED_DIR}/TEST_CHECKLIST.txt${NC}"

# Calculate elapsed time
END_TIME=$(date +%s)
ELAPSED=$((END_TIME - START_TIME))
MINUTES=$((ELAPSED / 60))
SECONDS=$((ELAPSED % 60))

echo ""
echo "=========================================="
echo -e "${CYAN}Test Suite Complete!${NC}"
echo "=========================================="
echo "Total time: ${MINUTES}m ${SECONDS}s"
echo ""
echo -e "${GREEN}Next Steps:${NC}"
echo "1. Start the Mac OS 9 emulator:"
echo "   ${SCRIPTS_DIR}/start-macos9.sh"
echo ""
echo "2. Open the shared folder and test all 4 binaries"
echo ""
echo "3. Use the checklist at:"
echo "   ${SHARED_DIR}/TEST_CHECKLIST.txt"
echo ""
echo "4. Review detailed analysis in:"
echo "   ${OUTPUT_DIR}/comparison/"
echo ""

# Final status
all_good=true
if [ ! -f "${SHARED_DIR}/MinimalTest_LLVM" ] || [ ! -f "${SHARED_DIR}/MinimalTest_Retro68" ]; then
    all_good=false
fi

if $all_good; then
    echo -e "${GREEN}✓ All critical files created successfully!${NC}"
    echo -e "${GREEN}✓ Ready for Mac OS 9 testing!${NC}"
    exit 0
else
    echo -e "${YELLOW}⚠ Some files missing, but test suite completed${NC}"
    exit 1
fi