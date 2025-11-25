# Unified Makefile for Mac OS Classic LLVM Toolchain
# Usage: make [target] [NAME=test_name]

# Directories
BUILD_DIR := $(CURDIR)/build
SCRIPTS_DIR := $(CURDIR)/scripts
TEST_DIR := $(CURDIR)/macos-classic/test-programs
RESOURCES_DIR := $(CURDIR)/macos-classic/resources
SHARED_DIR := $(CURDIR)/shared
RUNTIME_DIR := $(BUILD_DIR)/lib/clang/20/lib/macosclassic

# Tools
CLANG := $(BUILD_DIR)/bin/clang
CLANGXX := $(BUILD_DIR)/bin/clang++
LLD := $(BUILD_DIR)/bin/ld.lld
READOBJ := $(BUILD_DIR)/bin/llvm-readobj
OBJDUMP := $(BUILD_DIR)/bin/llvm-objdump

# Default test name
NAME ?= minimal_test

.PHONY: all help lld clang runtime macos-classic llvm test test-all prepare \
        clean-tests clean-shared info

# Default target
all: help

help:
	@echo "Mac OS Classic LLVM Toolchain - Build System"
	@echo "============================================="
	@echo ""
	@echo "LLVM Components:"
	@echo "  make lld              Rebuild linker (after Writer.cpp changes)"
	@echo "  make clang            Rebuild compiler"
	@echo "  make runtime          Rebuild Mac OS Classic runtime builtins"
	@echo "  make macos-classic    Rebuild lld + runtime"
	@echo "  make llvm             Full LLVM build (slow)"
	@echo ""
	@echo "Test Programs:"
	@echo "  make test NAME=<name>     Build a single test program"
	@echo "  make test-runtime NAME=<name>  Build with C runtime (uses main())"
	@echo "  make test-cxx NAME=<name>      Build C++ with runtime"
	@echo "  make test-all         Build all test programs"
	@echo "  make prepare NAME=<name>   Prepare for Mac OS 9 (copy to shared/)"
	@echo ""
	@echo "Utilities:"
	@echo "  make info NAME=<name>     Show PEF info for a binary"
	@echo "  make disasm NAME=<name>   Disassemble a binary"
	@echo ""
	@echo "Cleanup:"
	@echo "  make clean-tests      Remove test build artifacts"
	@echo "  make clean-shared     Remove shared directory contents"
	@echo ""
	@echo "Examples:"
	@echo "  make lld && make test NAME=beep_simple"
	@echo "  make test-runtime NAME=beep_test_main && make prepare NAME=beep_test_main"

# ============================================================================
# LLVM Components - Incremental Rebuilds
# ============================================================================

lld:
	@echo "Rebuilding LLD..."
	@cd $(BUILD_DIR) && ninja lld
	@echo "✓ LLD rebuilt"

clang:
	@echo "Rebuilding Clang..."
	@cd $(BUILD_DIR) && ninja clang
	@echo "✓ Clang rebuilt"

runtime:
	@echo "Building Mac OS Classic runtime builtins..."
	@cd $(BUILD_DIR) && ninja compiler-rt-builtins-powerpc-apple-macos-9 2>/dev/null || \
		echo "Note: Runtime target may not exist yet"
	@mkdir -p $(RUNTIME_DIR)
	@if ls $(BUILD_DIR)/lib/clang/20/lib/powerpc-apple-macos-9/*.o 1>/dev/null 2>&1; then \
		cp $(BUILD_DIR)/lib/clang/20/lib/powerpc-apple-macos-9/*.o $(RUNTIME_DIR)/; \
		echo "✓ Runtime builtins rebuilt and injected"; \
	else \
		echo "Note: No runtime .o files found to inject"; \
	fi

macos-classic: lld runtime
	@echo "✓ All Mac OS Classic components rebuilt"

llvm:
	@echo "Full LLVM build (this takes a while)..."
	@cd $(BUILD_DIR) && ninja
	@echo "✓ Full LLVM build complete"

# ============================================================================
# Test Program Building
# ============================================================================

test:
	@if [ ! -f "$(TEST_DIR)/$(NAME).c" ]; then \
		echo "Error: Source file not found: $(TEST_DIR)/$(NAME).c"; \
		exit 1; \
	fi
	@echo "Building $(NAME)..."
	@cd $(TEST_DIR) && $(CLANG) --target=powerpc-apple-classic \
		-ffreestanding -nostdlib -nostdinc -c $(NAME).c -o $(NAME).o
	@cd $(TEST_DIR) && $(LLD) -flavor pef -e __start $(NAME).o \
		-lInterfaceLib -o $(NAME).pef
	@echo "✓ Built $(TEST_DIR)/$(NAME).pef"

test-runtime:
	@if [ ! -f "$(TEST_DIR)/$(NAME).c" ]; then \
		echo "Error: Source file not found: $(TEST_DIR)/$(NAME).c"; \
		exit 1; \
	fi
	@echo "Building $(NAME) with C runtime..."
	@cd $(TEST_DIR) && $(CLANG) --target=powerpc-apple-classic \
		$(NAME).c -lInterfaceLib -o $(NAME).pef
	@echo "✓ Built $(TEST_DIR)/$(NAME).pef (with runtime)"

test-cxx:
	@if [ ! -f "$(TEST_DIR)/$(NAME).cpp" ]; then \
		echo "Error: Source file not found: $(TEST_DIR)/$(NAME).cpp"; \
		exit 1; \
	fi
	@echo "Building $(NAME) (C++)..."
	@cd $(TEST_DIR) && $(CLANGXX) --target=powerpc-apple-classic \
		-fno-exceptions -fno-rtti \
		$(NAME).cpp -lInterfaceLib -o $(NAME).pef
	@echo "✓ Built $(TEST_DIR)/$(NAME).pef (C++)"

test-all:
	@echo "Building all test programs..."
	@for src in $(TEST_DIR)/*.c; do \
		name=$$(basename $$src .c); \
		echo "  Building $$name..."; \
		$(MAKE) -s test NAME=$$name 2>/dev/null || echo "    Failed: $$name"; \
	done
	@echo "✓ All tests built"

# ============================================================================
# Prepare for Mac OS 9
# ============================================================================

prepare:
	@if [ ! -f "$(TEST_DIR)/$(NAME).pef" ]; then \
		echo "Error: PEF not found: $(TEST_DIR)/$(NAME).pef"; \
		echo "Run 'make test NAME=$(NAME)' first"; \
		exit 1; \
	fi
	@mkdir -p $(SHARED_DIR)
	@cp $(TEST_DIR)/$(NAME).pef $(SHARED_DIR)/$(NAME)
	@# Try to apply resource fork from reference binary
	@REF="$(RESOURCES_DIR)/$(NAME)_codewarrior"; \
	if [ ! -f "$$REF" ]; then REF="$(RESOURCES_DIR)/beep_test_codewarrior"; fi; \
	if [ -f "$$REF" ]; then \
		cp "$$REF/..namedfork/rsrc" "$(SHARED_DIR)/$(NAME)/..namedfork/rsrc" 2>/dev/null || true; \
		echo "✓ Resource fork applied"; \
	else \
		echo "Note: No reference binary found for resource fork"; \
	fi
	@SetFile -t APPL -c '????' "$(SHARED_DIR)/$(NAME)" 2>/dev/null || true
	@echo "✓ Prepared $(SHARED_DIR)/$(NAME) for Mac OS 9"

# ============================================================================
# Utilities
# ============================================================================

info:
	@if [ ! -f "$(TEST_DIR)/$(NAME).pef" ]; then \
		echo "Error: PEF not found: $(TEST_DIR)/$(NAME).pef"; \
		exit 1; \
	fi
	@$(READOBJ) --pef-header $(TEST_DIR)/$(NAME).pef

disasm:
	@if [ ! -f "$(TEST_DIR)/$(NAME).pef" ]; then \
		echo "Error: PEF not found: $(TEST_DIR)/$(NAME).pef"; \
		exit 1; \
	fi
	@$(OBJDUMP) --disassemble $(TEST_DIR)/$(NAME).pef

# ============================================================================
# Cleanup
# ============================================================================

clean-tests:
	@echo "Cleaning test build artifacts..."
	@rm -f $(TEST_DIR)/*.o $(TEST_DIR)/*.pef
	@echo "✓ Test artifacts cleaned"

clean-shared:
	@echo "Cleaning shared directory..."
	@rm -rf $(SHARED_DIR)/*
	@echo "✓ Shared directory cleaned"
