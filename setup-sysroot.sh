#!/bin/bash
# Setup script for Classic Mac OS sysroot
# Populates build/lib/clang-runtimes/powerpc-apple-classic/ with SDK files
#
# Part of LLVM PEF Linker for Classic Mac OS PowerPC
# This is an independent toolchain implementation, not affiliated with Retro68

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SYSROOT_DIR="$SCRIPT_DIR/build/lib/clang-runtimes/powerpc-apple-classic"

echo "========================================="
echo "Classic Mac OS Sysroot Setup"
echo "LLVM PEF Linker Toolchain"
echo "========================================="
echo ""

# Create directories
echo "Creating sysroot directories..."
mkdir -p "$SYSROOT_DIR/include"
mkdir -p "$SYSROOT_DIR/lib"

# Detect SDK source
SDK_SOURCE=""

if [ -n "$CLASSIC_MACOS_SDK" ] && [ -d "$CLASSIC_MACOS_SDK" ]; then
    SDK_SOURCE="$CLASSIC_MACOS_SDK"
    echo "Using SDK from CLASSIC_MACOS_SDK: $CLASSIC_MACOS_SDK"
else
    echo ""
    echo "ERROR: Cannot find Classic Mac OS SDK"
    echo ""
    echo "Please set the CLASSIC_MACOS_SDK environment variable to point to"
    echo "the Universal Interfaces & Libraries directory:"
    echo ""
    echo "  export CLASSIC_MACOS_SDK=/path/to/InterfacesAndLibraries"
    echo "  ./setup-sysroot.sh"
    echo ""
    echo "You can obtain the Classic Mac OS SDK from:"
    echo "  - Apple's Universal Interfaces & Libraries (recommended)"
    echo "  - MPW SDK from Apple"
    echo "  - Any compatible Classic Mac OS SDK distribution"
    echo ""
    echo "Expected directory structure:"
    echo "  \$CLASSIC_MACOS_SDK/Interfaces/CIncludes/    (C headers)"
    echo "  \$CLASSIC_MACOS_SDK/Libraries/SharedLibraries/ (PEF libraries)"
    echo ""
    echo "Or manually copy files to:"
    echo "  $SYSROOT_DIR/include/  (C headers)"
    echo "  $SYSROOT_DIR/lib/      (PEF libraries)"
    echo ""
    exit 1
fi

# Copy headers
echo ""
echo "Copying C headers..."
HEADERS_SRC="$SDK_SOURCE/Interfaces/CIncludes"
if [ -d "$HEADERS_SRC" ]; then
    cp -r "$HEADERS_SRC/"* "$SYSROOT_DIR/include/"
    HEADER_COUNT=$(find "$SYSROOT_DIR/include" -type f | wc -l)
    echo "  Copied $HEADER_COUNT header files"
else
    echo "  WARNING: Headers not found at $HEADERS_SRC"
fi

# Copy libraries
echo ""
echo "Copying PEF libraries..."
LIBS_COPIED=0

# SharedLibraries directory
LIBS_SRC="$SDK_SOURCE/Libraries/SharedLibraries"
if [ -d "$LIBS_SRC" ]; then
    # Copy all files (some don't have extensions)
    find "$LIBS_SRC" -maxdepth 1 -type f -exec cp {} "$SYSROOT_DIR/lib/" \;
    LIBS_COPIED=$(find "$SYSROOT_DIR/lib" -type f | wc -l)
fi

# StubLibraries directory (if present)
STUBS_SRC="$SDK_SOURCE/Libraries/SharedLibraries/StubLibraries"
if [ -d "$STUBS_SRC" ]; then
    find "$STUBS_SRC" -type f -exec cp {} "$SYSROOT_DIR/lib/" \;
    LIBS_COPIED=$(find "$SYSROOT_DIR/lib" -type f | wc -l)
fi

echo "  Copied $LIBS_COPIED library files"

# Also copy the minimal test libraries to lld/test/PEF/Inputs/lib
echo ""
echo "Copying minimal libraries to test directory..."
TEST_LIB_DIR="$SCRIPT_DIR/lld/test/PEF/Inputs/lib"
mkdir -p "$TEST_LIB_DIR"

if [ -f "$SYSROOT_DIR/lib/InterfaceLib" ]; then
    cp "$SYSROOT_DIR/lib/InterfaceLib" "$TEST_LIB_DIR/"
    echo "  Copied InterfaceLib to test directory"
fi

if [ -f "$SYSROOT_DIR/lib/MathLib" ]; then
    cp "$SYSROOT_DIR/lib/MathLib" "$TEST_LIB_DIR/"
    echo "  Copied MathLib to test directory"
fi

if [ -f "$SYSROOT_DIR/lib/StdCLib" ]; then
    cp "$SYSROOT_DIR/lib/StdCLib" "$TEST_LIB_DIR/"
    echo "  Copied StdCLib to test directory"
fi

# Verify installation
echo ""
echo "Verifying installation..."
ERRORS=0

if [ ! -f "$SYSROOT_DIR/include/MacTypes.h" ]; then
    echo "  ERROR: MacTypes.h not found in include/"
    ERRORS=$((ERRORS + 1))
fi

if [ ! -f "$SYSROOT_DIR/lib/InterfaceLib" ]; then
    echo "  ERROR: InterfaceLib not found in lib/"
    ERRORS=$((ERRORS + 1))
fi

if [ $ERRORS -eq 0 ]; then
    echo "  Verification passed!"
else
    echo "  Verification failed with $ERRORS errors"
    exit 1
fi

# Summary
echo ""
echo "========================================="
echo "Sysroot setup complete!"
echo "========================================="
echo ""
echo "Location: $SYSROOT_DIR"
echo ""
echo "Headers: $SYSROOT_DIR/include/"
echo "Libraries: $SYSROOT_DIR/lib/"
echo ""
echo "LLVM PEF Linker Toolchain for Classic Mac OS PowerPC"
echo ""
echo "The clang driver will automatically find this sysroot when building"
echo "for target: powerpc-apple-classic"
echo ""
echo "MacHeadersCompat.h is automatically included to provide compatibility"
echo "with Classic Mac OS Universal Interfaces headers."
echo ""
echo "Example usage:"
echo "  clang -target powerpc-apple-classic -c hello.c -o hello.o"
echo "  lld -flavor pef hello.o -o hello.pef -lInterfaceLib"
echo ""
echo "For more information about the LLVM PEF linker implementation,"
echo "see: lld/PEF/README.md"
echo ""
