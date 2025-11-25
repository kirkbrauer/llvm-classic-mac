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

# Check for git
if ! command -v git &> /dev/null; then
    echo "Error: git is not installed"
    case "$OS_TYPE" in
        Darwin)
            echo "Install with: brew install git"
            ;;
        Linux)
            echo "Install with: sudo apt-get install git"
            ;;
    esac
    exit 1
fi

# Dynamic path resolution - use WORKSPACE_ROOT if set, otherwise use parent of current directory
WORKSPACE_ROOT="${WORKSPACE_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
LLVM_DIR="${LLVM_DIR:-$WORKSPACE_ROOT/llvm-project}"
LLVM_VERSION="llvmorg-20.1.8"

# Clone LLVM if it doesn't exist
if [ ! -d "$LLVM_DIR" ]; then
    echo "📥 Cloning LLVM repository..."
    git clone https://github.com/llvm/llvm-project.git "$LLVM_DIR"
else
    echo "✅ LLVM repository already exists"
fi

# Checkout correct version
cd "$LLVM_DIR"
CURRENT_VERSION=$(git describe --tags 2>/dev/null || echo "unknown")
if [ "$CURRENT_VERSION" != "$LLVM_VERSION" ]; then
    echo "📌 Checking out $LLVM_VERSION..."
    git fetch --tags
    git checkout "$LLVM_VERSION"
else
    echo "✅ Already on $LLVM_VERSION"
fi

echo ""
echo "LLVM setup complete!"
echo "To build LLVM, run: bash build-llvm.sh"