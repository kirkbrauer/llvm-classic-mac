#!/bin/bash
set -e

# Detect platform
OS_TYPE=$(uname -s)
case "$OS_TYPE" in
    Darwin)
        echo "Running on macOS"
        ;;
    Linux)
        echo "Running on Linux"
        ;;
    *)
        echo "Warning: Unknown OS type: $OS_TYPE"
        ;;
esac

# Check dependencies
MISSING_DEPS=()
if ! command -v cmake &> /dev/null; then
    MISSING_DEPS+=("cmake")
fi
if ! command -v ninja &> /dev/null; then
    MISSING_DEPS+=("ninja")
fi

if [ ${#MISSING_DEPS[@]} -gt 0 ]; then
    echo "Error: Missing required dependencies: ${MISSING_DEPS[*]}"
    case "$OS_TYPE" in
        Darwin)
            echo "Install with: brew install ${MISSING_DEPS[*]}"
            ;;
        Linux)
            echo "Install with: sudo apt-get install ${MISSING_DEPS[*]}"
            ;;
    esac
    exit 1
fi

# Check for ccache (optional)
CCACHE_AVAILABLE=false
if command -v ccache &> /dev/null; then
    CCACHE_AVAILABLE=true
    echo "ccache found - will use for faster rebuilds"
else
    echo "ccache not found - builds will be slower (optional: install with brew/apt)"
fi

# Dynamic path resolution
WORKSPACE_ROOT="${WORKSPACE_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
LLVM_DIR="${LLVM_DIR:-$WORKSPACE_ROOT/llvm-project}"
BUILD_DIR="$LLVM_DIR/build"

# Detect host architecture
ARCH=$(uname -m)
case "$ARCH" in
    x86_64)
        HOST_ARCH="X86"
        ;;
    aarch64|arm64)
        HOST_ARCH="AArch64"
        ;;
    *)
        echo "❌ Unsupported architecture: $ARCH"
        exit 1
        ;;
esac

echo "═══════════════════════════════════════════════════════"
echo "  Building LLVM"
echo "═══════════════════════════════════════════════════════"
echo ""
echo "Host Architecture: $HOST_ARCH"
echo "Targets: $HOST_ARCH;PowerPC"
echo "Build Directory: $BUILD_DIR"
echo ""

if [ ! -d "$LLVM_DIR" ]; then
    echo "❌ LLVM directory not found. Run setup-llvm.sh first."
    exit 1
fi

# Check if already built
if [ -f "$BUILD_DIR/bin/llvm-config" ]; then
    echo "⚠️  LLVM already built at $BUILD_DIR"

    # Check if running interactively
    if [ -t 0 ]; then
        read -p "Rebuild from scratch? (y/N): " -n 1 -r
        echo
        if [[ $REPLY =~ ^[Yy]$ ]]; then
            echo "🗑️  Removing old build..."
            rm -rf "$BUILD_DIR"
        else
            echo "Keeping existing build. To rebuild, delete $BUILD_DIR or set FORCE_REBUILD=1"
            exit 0
        fi
    else
        # Non-interactive mode - check for FORCE_REBUILD env var
        if [ "${FORCE_REBUILD:-0}" = "1" ]; then
            echo "FORCE_REBUILD set - removing old build..."
            rm -rf "$BUILD_DIR"
        else
            echo "Keeping existing build. To rebuild, delete $BUILD_DIR or set FORCE_REBUILD=1"
            exit 0
        fi
    fi
fi

# Create build directory
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

echo "⚙️  Configuring LLVM..."

# Build cmake command with optional ccache
CMAKE_ARGS=(
    -G Ninja ../llvm
    -DCMAKE_BUILD_TYPE=Release
    -DLLVM_ENABLE_PROJECTS="clang;lld"
    -DLLVM_TARGETS_TO_BUILD="${HOST_ARCH};PowerPC"
    -DLLVM_ENABLE_ASSERTIONS=OFF
    -DCMAKE_INSTALL_PREFIX="$LLVM_DIR/install"
    -DLLVM_PARALLEL_LINK_JOBS=2
)

# Add ccache if available
if [ "$CCACHE_AVAILABLE" = true ]; then
    CMAKE_ARGS+=(-DLLVM_CCACHE_BUILD=ON)
fi

cmake "${CMAKE_ARGS[@]}"

echo ""
echo "🔨 Building LLVM (this will take 30-45 minutes)..."
echo "⏳ Started at: $(date)"
START_TIME=$(date +%s)

ninja -j8

END_TIME=$(date +%s)
DURATION=$((END_TIME - START_TIME))
MINUTES=$((DURATION / 60))
SECONDS=$((DURATION % 60))

echo ""
echo "✅ LLVM build complete!"
echo "⏱️  Build time: ${MINUTES}m ${SECONDS}s"
echo ""

# Verify build
echo "Verifying build..."
if [ -f "$BUILD_DIR/bin/llvm-config" ]; then
    echo "LLVM Version: $($BUILD_DIR/bin/llvm-config --version)"
    echo "LLVM Targets: $($BUILD_DIR/bin/llvm-config --targets-built)"
    echo ""
    echo "LLVM tools available at: $BUILD_DIR/bin/"
else
    echo "❌ Build verification failed!"
    exit 1
fi