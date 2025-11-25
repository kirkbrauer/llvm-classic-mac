#!/bin/bash
# fix-line-endings.sh - Convert source files to Mac OS 9 CR line endings

set -e

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

# Dynamic path resolution
WORKSPACE_ROOT="${WORKSPACE_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
PROJECTS_DIR="${PROJECTS_DIR:-$WORKSPACE_ROOT/shared/projects}"

# Function to convert file to CR line endings
convert_to_cr() {
    local file="$1"
    local temp_file="${file}.tmp"

    # Check if file exists
    if [ ! -f "$file" ]; then
        echo -e "${RED}Error: File not found: $file${NC}"
        return 1
    fi

    # Convert LF or CRLF to CR
    # Method: tr to convert \n to \r
    tr '\n' '\r' < "$file" > "$temp_file"

    # Check if conversion was successful
    if [ $? -eq 0 ]; then
        mv "$temp_file" "$file"
        echo -e "${GREEN}✓ Converted: $file${NC}"
        return 0
    else
        rm -f "$temp_file"
        echo -e "${RED}✗ Failed: $file${NC}"
        return 1
    fi
}

# Function to check line ending type
check_line_ending() {
    local file="$1"

    # Check for different line endings
    if file "$file" | grep -q "CRLF"; then
        echo "CRLF (Windows)"
    elif file "$file" | grep -q "CR line"; then
        echo "CR (Mac)"
    elif file "$file" | grep -q "ASCII text"; then
        # Could be LF (Unix) or no line endings
        if grep -q $'\r' "$file"; then
            echo "CR (Mac)"
        else
            echo "LF (Unix)"
        fi
    else
        echo "Unknown"
    fi
}

# Main script
echo -e "${GREEN}Mac OS 9 Line Ending Converter${NC}"
echo ""

# Check if argument provided
if [ $# -eq 0 ]; then
    # No argument - convert all C/C++ files in projects
    echo "Converting all C/C++ source files in: $PROJECTS_DIR"
    echo ""

    # Find all .c, .cpp, .h files
    find "$PROJECTS_DIR" -type f \( -name "*.c" -o -name "*.cpp" -o -name "*.h" -o -name "*.hpp" \) \
        ! -path "*/.AppleDouble/*" \
        ! -path "*/build/*" \
        -print0 | while IFS= read -r -d '' file; do

        # Check current line ending
        current=$(check_line_ending "$file")

        if [ "$current" != "CR (Mac)" ]; then
            echo -e "${YELLOW}$file: $current → CR${NC}"
            convert_to_cr "$file"
        fi
    done

    echo ""
    echo -e "${GREEN}Conversion complete!${NC}"

elif [ -f "$1" ]; then
    # Single file provided
    file="$1"
    current=$(check_line_ending "$file")

    echo "File: $file"
    echo "Current: $current"

    if [ "$current" = "CR (Mac)" ]; then
        echo -e "${GREEN}Already using CR line endings${NC}"
    else
        echo -e "${YELLOW}Converting to CR...${NC}"
        convert_to_cr "$file"
        echo -e "${GREEN}Done!${NC}"
    fi

elif [ -d "$1" ]; then
    # Directory provided
    dir="$1"
    echo "Converting all C/C++ files in: $dir"
    echo ""

    find "$dir" -type f \( -name "*.c" -o -name "*.cpp" -o -name "*.h" -o -name "*.hpp" \) \
        ! -path "*/.AppleDouble/*" \
        ! -path "*/build/*" \
        -print0 | while IFS= read -r -d '' file; do

        current=$(check_line_ending "$file")

        if [ "$current" != "CR (Mac)" ]; then
            echo -e "${YELLOW}$file: $current → CR${NC}"
            convert_to_cr "$file"
        fi
    done

    echo ""
    echo -e "${GREEN}Conversion complete!${NC}"

else
    echo -e "${RED}Error: File or directory not found: $1${NC}"
    echo ""
    echo "Usage:"
    echo "  $0                  # Convert all files in: $PROJECTS_DIR"
    echo "  $0 <file>           # Convert specific file"
    echo "  $0 <directory>      # Convert all files in directory"
    echo ""
    echo "Environment variables:"
    echo "  PROJECTS_DIR        # Override default projects directory"
    echo "  WORKSPACE_ROOT      # Override workspace root (default: parent of scripts/)"
    exit 1
fi
