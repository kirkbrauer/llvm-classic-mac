#!/bin/bash
#
# Generate cfrg Resource with Correct Fragment Name
#
# Usage: ./generate_cfrg_resource.sh <binary_path> [fragment_name]
#
# This script generates a proper Mac OS 9 cfrg (Code Fragment) resource
# with the correct fragment name, matching CodeWarrior's structure.
#
# If fragment_name is not provided, uses the basename of the binary.
#

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
BLUE='\033[0;36m'
NC='\033[0m' # No Color

# Check arguments
if [ $# -lt 1 ]; then
    echo "Usage: $0 <binary_path> [fragment_name]"
    echo ""
    echo "Example:"
    echo "  $0 output/llvm/minimal_test.pef"
    echo "  $0 output/llvm/minimal_test.pef \"MyApp\""
    exit 1
fi

BINARY_PATH="$1"
FRAGMENT_NAME="${2:-}"

# Check if binary exists
if [ ! -f "$BINARY_PATH" ]; then
    echo -e "${RED}Error: Binary not found: $BINARY_PATH${NC}"
    exit 1
fi

# If no fragment name provided, use basename without extension
if [ -z "$FRAGMENT_NAME" ]; then
    FRAGMENT_NAME=$(basename "$BINARY_PATH" | sed 's/\.[^.]*$//')
fi

echo -e "${BLUE}=== cfrg Resource Generator ===${NC}"
echo "Binary: $BINARY_PATH"
echo "Fragment Name: $FRAGMENT_NAME"
echo ""

# Create temporary directory for resource files
TEMP_DIR=$(mktemp -d)
trap "rm -rf $TEMP_DIR" EXIT

RESOURCE_FILE="$TEMP_DIR/cfrg_resource.r"
RESOURCE_OUTPUT="$TEMP_DIR/resources.rsrc"

# Convert fragment name to hex for the resource
FRAGMENT_NAME_HEX=$(echo -n "$FRAGMENT_NAME" | xxd -p | tr -d '\n' | sed 's/../& /g' | sed 's/ $//')
FRAGMENT_NAME_LENGTH=$(echo -n "$FRAGMENT_NAME" | wc -c | tr -d ' ')
FRAGMENT_NAME_LENGTH_HEX=$(printf "%02X" $FRAGMENT_NAME_LENGTH)

echo "Generating resource file..."
echo "  Fragment name length: $FRAGMENT_NAME_LENGTH bytes"

# Generate the .r file with proper cfrg resource
cat > "$RESOURCE_FILE" << 'EOFTEMPLATE'
/*
 * Automatically generated cfrg resource
 * Matches CodeWarrior structure for Mac OS 9 compatibility
 */

/* Code Fragment Resource (cfrg) */
data 'cfrg' (0) {
    /* Header (32 bytes) */
    $"0000 0000"     /* Reserved1: 0 */
    $"0000 0000"     /* Reserved2: 0 */
    $"0000 0001"     /* Version: 1 */
    $"0000 0000"     /* Reserved3: 0 */
    $"0000 0000"     /* Reserved4: 0 */
    $"0000 0000"     /* Reserved5: 0 */
    $"0000 0000"     /* Reserved6: 0 */
    $"0000 0001"     /* Fragment count: 1 */

    /* Fragment descriptor (starts at offset 32) */
    $"7077 7063"     /* Architecture: 'pwpc' (PowerPC) */
    $"0000 0000"     /* Update level: 0 (MATCHING CODEWARRIOR!) */
    $"0000 0000"     /* Current version: 0 */
    $"0000 0000"     /* Old def version: 0 */
    $"0001 0000"     /* Application stack size: 65536 bytes (0x00010000) */
    $"0000 0101"     /* App sub folder (0) + Usage/Location/Flags */
    $"0000 0000"     /* Offset: 0 (from start of data fork) */
    $"0000 0000"     /* Length: 0 (entire data fork) */
    $"0000 0000"     /* Reserved7: 0 */
    $"0000 003C"     /* Reserved8: 0x3C */

    /* Fragment name (Pascal string) */
EOFTEMPLATE

# Add the fragment name hex
echo "    \$\"${FRAGMENT_NAME_LENGTH_HEX}${FRAGMENT_NAME_HEX}00\"  /* Fragment name: \"$FRAGMENT_NAME\" */" >> "$RESOURCE_FILE"

# Close the cfrg resource and add SIZE resource
cat >> "$RESOURCE_FILE" << 'EOF'
};

/* SIZE resource - memory requirements */
data 'SIZE' (-1) {
    $"58E0"          /* Flags: is32BitCompatible, accepts suspend/resume, can background */
    $"0006 0000"     /* Preferred memory: 384K */
    $"0006 0000"     /* Minimum memory: 384K */
};

/* Version resource */
data 'vers' (1) {
    $"0100"          /* Major: 1, Minor: 0 */
    $"8000"          /* Development stage: final */
    $"0000"          /* Prerelease revision: 0 */
    $"0000"          /* Region: 0 */
    $"0331 2E30"     /* Short version string length and "1.0" */
    $"1C31 2E30 2C20" /* Long version string start "1.0, " */
    $"4D61 6320 4F53" /* "Mac OS" */
    $"2050 6F77 6572" /* " Power" */
    $"5043 2041 7070" /* "PC App" */
};
EOF

echo -e "${GREEN}✓ Resource file generated${NC}"

# Compile the resource directly onto the binary with Rez
echo "Compiling and applying resource with Rez..."

# Rez writes to the resource fork of the output file
# We need to create a temporary binary copy
TEMP_BINARY="$TEMP_DIR/temp_binary"
cp "$BINARY_PATH" "$TEMP_BINARY"

if Rez "$RESOURCE_FILE" -o "$TEMP_BINARY" 2>&1; then
    # Success - now copy back to original location
    cp "$TEMP_BINARY" "$BINARY_PATH"
    # Copy the resource fork
    if [ -f "$TEMP_BINARY/..namedfork/rsrc" ]; then
        cp "$TEMP_BINARY/..namedfork/rsrc" "$BINARY_PATH/..namedfork/rsrc"
    fi
else
    echo -e "${RED}✗ Rez compilation failed${NC}"
    cat "$RESOURCE_FILE"
    exit 1
fi

# Set file type and creator
# APPL = Application, ???? = unknown creator
xattr -wx com.apple.FinderInfo "4150504C3F3F3F3F010000000000000000000000000000000000000000000000" "$BINARY_PATH" 2>/dev/null || true

echo -e "${GREEN}✓ Resources applied${NC}"

# Verify with DeRez
echo ""
echo "Verifying resources..."
if DeRez "$BINARY_PATH" 2>&1 | grep -q "cfrg"; then
    echo -e "${GREEN}✓ cfrg resource verified${NC}"

    # Show the fragment name from the resource
    echo ""
    echo "Fragment name in cfrg resource:"
    DeRez "$BINARY_PATH" 2>&1 | grep -A 20 "data 'cfrg'" | tail -5
else
    echo -e "${RED}✗ cfrg resource not found${NC}"
    exit 1
fi

# Show file attributes
echo ""
echo "File attributes:"
ls -la@ "$BINARY_PATH" | head -2

echo ""
echo -e "${GREEN}=== Resource generation complete! ===${NC}"
echo ""
echo "Binary ready for Mac OS 9:"
echo "  $BINARY_PATH"
echo "  Fragment name: $FRAGMENT_NAME"
echo "  Update level: 0 (CodeWarrior-compatible)"
