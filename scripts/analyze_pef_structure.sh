#!/bin/bash

# analyze_pef_structure.sh
# Analyzes PEF structure of binaries from both LLVM and Retro68 toolchains
# Provides detailed comparison of headers, sections, and loader information

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo "=========================================="
echo "PEF Structure Analysis Script"
echo "=========================================="

# Setup paths - dynamically determine repo root from script location
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
PARENT_ROOT="$(cd "${REPO_ROOT}/.." && pwd)"

OUTPUT_DIR="${REPO_ROOT}/output"
COMPARISON_DIR="${OUTPUT_DIR}/comparison"

# LLVM toolchain paths
LLVM_BIN="${REPO_ROOT}/build/bin"
LLVM_READOBJ="${LLVM_BIN}/llvm-readobj"

# Retro68 toolchain paths (in parent directory)
RETRO68_BIN="${PARENT_ROOT}/Retro68-build/toolchain/bin"
RETRO68_OBJDUMP="${RETRO68_BIN}/powerpc-apple-macos-objdump"

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
echo "Analyzing LLVM PEF structure"
echo "=========================================="

# Get PEF headers from LLVM binary
echo "Extracting LLVM PEF headers..."
"${LLVM_READOBJ}" --pef-headers "${OUTPUT_DIR}/llvm/minimal_test.pef" \
    > "${COMPARISON_DIR}/llvm_pef_headers.txt" 2>&1 || {
    echo -e "${YELLOW}Note: --pef-headers flag might not be available, trying alternatives${NC}"
    "${LLVM_READOBJ}" --all "${OUTPUT_DIR}/llvm/minimal_test.pef" \
        > "${COMPARISON_DIR}/llvm_pef_headers.txt" 2>&1
}

echo "Created: ${COMPARISON_DIR}/llvm_pef_headers.txt"

# Get sections from LLVM binary
echo "Extracting LLVM PEF sections..."
"${LLVM_READOBJ}" --sections "${OUTPUT_DIR}/llvm/minimal_test.pef" \
    > "${COMPARISON_DIR}/llvm_pef_sections.txt" 2>&1

echo "Created: ${COMPARISON_DIR}/llvm_pef_sections.txt"

# Get symbols from LLVM binary
echo "Extracting LLVM symbols..."
"${LLVM_READOBJ}" --symbols "${OUTPUT_DIR}/llvm/minimal_test.pef" \
    > "${COMPARISON_DIR}/llvm_pef_symbols.txt" 2>&1

echo "Created: ${COMPARISON_DIR}/llvm_pef_symbols.txt"

echo ""
echo "=========================================="
echo "Analyzing Retro68 PEF structure"
echo "=========================================="

# Try llvm-readobj on Retro68 PEF
echo "Analyzing Retro68 PEF with llvm-readobj..."
"${LLVM_READOBJ}" --all "${OUTPUT_DIR}/retro68/minimal_test.pef" \
    > "${COMPARISON_DIR}/retro68_pef_headers.txt" 2>&1

echo "Created: ${COMPARISON_DIR}/retro68_pef_headers.txt"

"${LLVM_READOBJ}" --sections "${OUTPUT_DIR}/retro68/minimal_test.pef" \
    > "${COMPARISON_DIR}/retro68_pef_sections.txt" 2>&1

echo "Created: ${COMPARISON_DIR}/retro68_pef_sections.txt"

"${LLVM_READOBJ}" --symbols "${OUTPUT_DIR}/retro68/minimal_test.pef" \
    > "${COMPARISON_DIR}/retro68_pef_symbols.txt" 2>&1

echo "Created: ${COMPARISON_DIR}/retro68_pef_symbols.txt"

echo ""
echo "=========================================="
echo "Hexdump analysis of PEF headers"
echo "=========================================="

# Function to analyze PEF header
analyze_pef_header() {
    local file=$1
    local name=$2
    local output=$3

    echo "=== $name PEF Header Analysis ===" > "$output"
    echo "" >> "$output"

    # Get first 256 bytes for header analysis
    echo "Container Header (first 40 bytes):" >> "$output"
    xxd -l 40 "$file" >> "$output"
    echo "" >> "$output"

    # Extract key fields from PEF container header
    echo "Parsed Container Header:" >> "$output"

    # Magic (offset 0, 4 bytes) - should be 0x4A6F7921 "Joy!"
    magic=$(xxd -s 0 -l 4 -p "$file")
    echo "  Magic: 0x${magic} ($(echo $magic | xxd -r -p))" >> "$output"

    # Container ID (offset 4, 4 bytes)
    container_id=$(xxd -s 4 -l 4 -p "$file")
    echo "  Container ID: 0x${container_id}" >> "$output"

    # Architecture (offset 8, 4 bytes)
    arch=$(xxd -s 8 -l 4 -p "$file")
    echo "  Architecture: 0x${arch}" >> "$output"

    # Format version (offset 12, 4 bytes)
    version=$(xxd -s 12 -l 4 -p "$file")
    echo "  Format Version: 0x${version}" >> "$output"

    # Date/Time stamp (offset 16, 4 bytes)
    timestamp=$(xxd -s 16 -l 4 -p "$file")
    echo "  Timestamp: 0x${timestamp}" >> "$output"

    # Old definition version (offset 20, 4 bytes)
    old_def=$(xxd -s 20 -l 4 -p "$file")
    echo "  Old Def Version: 0x${old_def}" >> "$output"

    # Old implementation version (offset 24, 4 bytes)
    old_impl=$(xxd -s 24 -l 4 -p "$file")
    echo "  Old Impl Version: 0x${old_impl}" >> "$output"

    # Current version (offset 28, 4 bytes)
    current_ver=$(xxd -s 28 -l 4 -p "$file")
    echo "  Current Version: 0x${current_ver}" >> "$output"

    # Section count (offset 32, 2 bytes)
    section_count=$(xxd -s 32 -l 2 -p "$file")
    echo "  Section Count: 0x${section_count} ($(printf "%d" 0x${section_count}))" >> "$output"

    # Instantiated sections count (offset 34, 2 bytes)
    inst_sections=$(xxd -s 34 -l 2 -p "$file")
    echo "  Instantiated Sections: 0x${inst_sections} ($(printf "%d" 0x${inst_sections}))" >> "$output"

    # Reserved (offset 36, 4 bytes)
    reserved=$(xxd -s 36 -l 4 -p "$file")
    echo "  Reserved: 0x${reserved}" >> "$output"

    echo "" >> "$output"

    # Section headers start at offset 40
    echo "Section Headers (starting at offset 40):" >> "$output"
    xxd -s 40 -l 200 "$file" >> "$output"
    echo "" >> "$output"
}

# Analyze both PEF files
analyze_pef_header "${OUTPUT_DIR}/llvm/minimal_test.pef" "LLVM" \
    "${COMPARISON_DIR}/llvm_pef_hexdump.txt"

echo "Created: ${COMPARISON_DIR}/llvm_pef_hexdump.txt"

analyze_pef_header "${OUTPUT_DIR}/retro68/minimal_test.pef" "Retro68" \
    "${COMPARISON_DIR}/retro68_pef_hexdump.txt"

echo "Created: ${COMPARISON_DIR}/retro68_pef_hexdump.txt"

echo ""
echo "=========================================="
echo "Creating PEF structure comparison"
echo "=========================================="

# Create detailed comparison report
{
    echo "PEF Structure Comparison Report"
    echo "================================"
    echo ""
    echo "Generated: $(date)"
    echo ""

    echo "File Sizes:"
    echo "-----------"
    echo "LLVM PEF:    $(ls -lh "${OUTPUT_DIR}/llvm/minimal_test.pef" | awk '{print $5}')"
    echo "Retro68 PEF: $(ls -lh "${OUTPUT_DIR}/retro68/minimal_test.pef" | awk '{print $5}')"
    echo ""

    echo "PEF Magic Verification:"
    echo "-----------------------"
    # Check magic for both
    llvm_magic=$(xxd -s 0 -l 4 -p "${OUTPUT_DIR}/llvm/minimal_test.pef")
    retro68_magic=$(xxd -s 0 -l 4 -p "${OUTPUT_DIR}/retro68/minimal_test.pef")

    if [ "$llvm_magic" = "4a6f7921" ]; then
        echo "LLVM:    ✓ Valid PEF magic (0x4a6f7921 'Joy!')"
    else
        echo "LLVM:    ✗ Invalid PEF magic (0x${llvm_magic})"
    fi

    if [ "$retro68_magic" = "4a6f7921" ]; then
        echo "Retro68: ✓ Valid PEF magic (0x4a6f7921 'Joy!')"
    else
        echo "Retro68: ✗ Invalid PEF magic (0x${retro68_magic})"
    fi
    echo ""

    echo "Architecture Field:"
    echo "-------------------"
    llvm_arch=$(xxd -s 8 -l 4 -p "${OUTPUT_DIR}/llvm/minimal_test.pef")
    retro68_arch=$(xxd -s 8 -l 4 -p "${OUTPUT_DIR}/retro68/minimal_test.pef")

    echo "LLVM:    0x${llvm_arch} ($([ "$llvm_arch" = "70777063" ] && echo "PowerPC 'pwpc'" || echo "Unknown"))"
    echo "Retro68: 0x${retro68_arch} ($([ "$retro68_arch" = "70777063" ] && echo "PowerPC 'pwpc'" || echo "Unknown"))"
    echo ""

    echo "Section Count:"
    echo "--------------"
    llvm_sections=$(xxd -s 32 -l 2 -p "${OUTPUT_DIR}/llvm/minimal_test.pef")
    retro68_sections=$(xxd -s 32 -l 2 -p "${OUTPUT_DIR}/retro68/minimal_test.pef")

    echo "LLVM:    0x${llvm_sections} ($(printf "%d" 0x${llvm_sections}) sections)"
    echo "Retro68: 0x${retro68_sections} ($(printf "%d" 0x${retro68_sections}) sections)"
    echo ""

    echo "Entry Point Analysis:"
    echo "---------------------"
    # Look for __start in symbol tables
    echo "LLVM exports:"
    grep -i "__start\|entry" "${COMPARISON_DIR}/llvm_pef_symbols.txt" 2>/dev/null | head -5 || echo "  No __start found in symbols"
    echo ""
    echo "Retro68 exports:"
    grep -i "__start\|entry" "${COMPARISON_DIR}/retro68_pef_symbols.txt" 2>/dev/null | head -5 || echo "  No __start found in symbols"
    echo ""

    echo "Import Analysis (ExitToShell):"
    echo "-------------------------------"
    echo "LLVM imports:"
    grep -i "ExitToShell\|import" "${COMPARISON_DIR}/llvm_pef_symbols.txt" 2>/dev/null | head -5 || echo "  No ExitToShell found in symbols"
    echo ""
    echo "Retro68 imports:"
    grep -i "ExitToShell\|import" "${COMPARISON_DIR}/retro68_pef_symbols.txt" 2>/dev/null | head -5 || echo "  No ExitToShell found in symbols"
    echo ""

} > "${COMPARISON_DIR}/pef_structure_comparison.txt"

echo "Created: ${COMPARISON_DIR}/pef_structure_comparison.txt"

echo ""
echo "=========================================="
echo "Loader Section Analysis"
echo "=========================================="

# Function to find and analyze loader section
find_loader_section() {
    local file=$1
    local name=$2
    local output=$3

    echo "=== $name Loader Section Analysis ===" > "$output"
    echo "" >> "$output"

    # Read section count
    section_count_hex=$(xxd -s 32 -l 2 -p "$file")
    section_count=$(printf "%d" 0x${section_count_hex})

    echo "Total sections: $section_count" >> "$output"
    echo "" >> "$output"

    # Each section header is 28 bytes, starting at offset 40
    offset=40
    for i in $(seq 1 $section_count); do
        # Read section name (first 4 bytes of section header)
        section_name=$(xxd -s $offset -l 4 "$file" | cut -d' ' -f2)

        # Read section type (offset+4, 4 bytes)
        section_type=$(xxd -s $((offset+4)) -l 4 -p "$file")

        echo "Section $i:" >> "$output"
        echo "  Name offset: $section_name" >> "$output"
        echo "  Type: 0x${section_type}" >> "$output"

        # Check if this is the loader section (type 4)
        if [ "$section_type" = "00000004" ]; then
            echo "  >> This is the LOADER section! <<" >> "$output"

            # Get loader section offset and size
            loader_offset=$(xxd -s $((offset+8)) -l 4 -p "$file")
            loader_size=$(xxd -s $((offset+16)) -l 4 -p "$file")

            echo "  Loader offset: 0x${loader_offset}" >> "$output"
            echo "  Loader size: 0x${loader_size}" >> "$output"
            echo "" >> "$output"

            # Show first 256 bytes of loader section
            loader_offset_dec=$(printf "%d" 0x${loader_offset})
            echo "  Loader section contents (first 256 bytes):" >> "$output"
            xxd -s $loader_offset_dec -l 256 "$file" >> "$output"
        fi

        echo "" >> "$output"
        offset=$((offset + 28))
    done
}

# Analyze loader sections
find_loader_section "${OUTPUT_DIR}/llvm/minimal_test.pef" "LLVM" \
    "${COMPARISON_DIR}/llvm_loader_section.txt"

echo "Created: ${COMPARISON_DIR}/llvm_loader_section.txt"

find_loader_section "${OUTPUT_DIR}/retro68/minimal_test.pef" "Retro68" \
    "${COMPARISON_DIR}/retro68_loader_section.txt"

echo "Created: ${COMPARISON_DIR}/retro68_loader_section.txt"

echo ""
echo "=========================================="
echo "PEF Validation Checklist"
echo "=========================================="

# Create validation checklist
{
    echo "PEF Binary Validation Checklist"
    echo "================================"
    echo ""
    echo "Generated: $(date)"
    echo ""

    # Function to check validation points
    check_validation() {
        local file=$1
        local name=$2

        echo "$name Binary Validation:"
        echo "------------------------"

        # Check magic
        magic=$(xxd -s 0 -l 4 -p "$file")
        if [ "$magic" = "4a6f7921" ]; then
            echo "[✓] Valid PEF magic (Joy!)"
        else
            echo "[✗] Invalid PEF magic"
        fi

        # Check architecture
        arch=$(xxd -s 8 -l 4 -p "$file")
        if [ "$arch" = "70777063" ]; then
            echo "[✓] Valid PowerPC architecture"
        else
            echo "[✗] Invalid architecture"
        fi

        # Check section count
        sections=$(xxd -s 32 -l 2 -p "$file")
        section_num=$(printf "%d" 0x${sections})
        if [ $section_num -gt 0 ]; then
            echo "[✓] Has $section_num sections"
        else
            echo "[✗] No sections found"
        fi

        # Check file size
        size=$(stat -f%z "$file" 2>/dev/null || stat -c%s "$file" 2>/dev/null)
        if [ $size -lt 10240 ]; then
            echo "[✓] Reasonable size (${size} bytes < 10KB)"
        else
            echo "[⚠] Large size (${size} bytes)"
        fi

        # Check for loader section
        loader_type=$(xxd -s 44 -l 4 -p "$file")  # Assuming first section
        if grep -q "00000004" <<< "$loader_type"; then
            echo "[✓] Loader section present"
        else
            echo "[?] Loader section needs verification"
        fi

        echo ""
    }

    check_validation "${OUTPUT_DIR}/llvm/minimal_test.pef" "LLVM"
    check_validation "${OUTPUT_DIR}/retro68/minimal_test.pef" "Retro68"

    echo "Comparison Summary:"
    echo "-------------------"

    # Compare sizes
    llvm_size=$(stat -f%z "${OUTPUT_DIR}/llvm/minimal_test.pef" 2>/dev/null || stat -c%s "${OUTPUT_DIR}/llvm/minimal_test.pef" 2>/dev/null)
    retro68_size=$(stat -f%z "${OUTPUT_DIR}/retro68/minimal_test.pef" 2>/dev/null || stat -c%s "${OUTPUT_DIR}/retro68/minimal_test.pef" 2>/dev/null)

    size_diff=$((retro68_size - llvm_size))
    percent_diff=$((size_diff * 100 / llvm_size))

    echo "Size difference: ${size_diff} bytes (Retro68 is ${percent_diff}% larger)"

    # Compare section counts
    llvm_sec=$(printf "%d" 0x$(xxd -s 32 -l 2 -p "${OUTPUT_DIR}/llvm/minimal_test.pef"))
    retro68_sec=$(printf "%d" 0x$(xxd -s 32 -l 2 -p "${OUTPUT_DIR}/retro68/minimal_test.pef"))

    echo "Section count: LLVM=${llvm_sec}, Retro68=${retro68_sec}"

} > "${COMPARISON_DIR}/pef_validation_checklist.txt"

echo "Created: ${COMPARISON_DIR}/pef_validation_checklist.txt"

echo ""
echo "=========================================="
echo "PEF Analysis Complete"
echo "=========================================="
echo -e "${GREEN}✓ PEF structure analysis completed successfully${NC}"
echo ""
echo "Generated files in ${COMPARISON_DIR}:"
echo "  - llvm_pef_headers.txt         : LLVM PEF header details"
echo "  - llvm_pef_sections.txt        : LLVM section information"
echo "  - llvm_pef_symbols.txt         : LLVM symbol table"
echo "  - llvm_pef_hexdump.txt         : LLVM raw header analysis"
echo "  - llvm_loader_section.txt      : LLVM loader section details"
echo "  - retro68_pef_headers.txt      : Retro68 PEF header details"
echo "  - retro68_pef_sections.txt     : Retro68 section information"
echo "  - retro68_pef_symbols.txt      : Retro68 symbol table"
echo "  - retro68_pef_hexdump.txt      : Retro68 raw header analysis"
echo "  - retro68_loader_section.txt   : Retro68 loader section details"
echo "  - pef_structure_comparison.txt : Side-by-side comparison"
echo "  - pef_validation_checklist.txt : Validation results"
echo ""
echo "Next steps:"
echo "  1. Run './scripts/add_resource_fork.sh' to add resources"
echo "  2. Test binaries in SheepShaver emulator"
echo "  3. Review comparison files for any issues"