# Makefile for bkbtl-debugger
#
# Mirrors the Debug/Release configurations of bkbtldebug.vcxproj (VS2022).
#
# Usage:
#   make            # Release build (default)
#   make debug      # Debug build (-D_DEBUG, asserts enabled, no optimization)
#   make release    # Release build explicitly
#   make clean       # Remove build artifacts for both configs
#   make run         # Build (release) and run
#   make run-debug   # Build (debug) and run
#
# Binaries are placed in build/<config>/bkbtldebug

CXX      := g++
CXXSTD   := -std=c++17
TARGET   := bkbtldebug

SRCS := \
	bkbtldebug.cpp \
	Common.cpp \
	commands.cpp \
	Emulator.cpp \
	stdafx.cpp \
	emubase/Board.cpp \
	emubase/Disasm.cpp \
	emubase/Floppy.cpp \
	emubase/Processor.cpp \
	emubase/SoundAY.cpp

INCLUDES := -I.

WARNINGS := -Wall -Wextra -Wno-unused-parameter

# --- Configuration-specific flags -------------------------------------------

RELEASE_DIR   := build/release
DEBUG_DIR     := build/debug

RELEASE_FLAGS := -O2 -DNDEBUG
DEBUG_FLAGS   := -O0 -g -D_DEBUG

# --- Default target -----------------------------------------------------

.PHONY: all release debug clean run run-debug

all: release

release: $(RELEASE_DIR)/$(TARGET)

debug: $(DEBUG_DIR)/$(TARGET)

# --- Object file lists ----------------------------------------------------

RELEASE_OBJS := $(patsubst %.cpp,$(RELEASE_DIR)/%.o,$(SRCS))
DEBUG_OBJS   := $(patsubst %.cpp,$(DEBUG_DIR)/%.o,$(SRCS))

# --- Link -------------------------------------------------------------------

$(RELEASE_DIR)/$(TARGET): $(RELEASE_OBJS)
	$(CXX) $(RELEASE_OBJS) -o $@

$(DEBUG_DIR)/$(TARGET): $(DEBUG_OBJS)
	$(CXX) $(DEBUG_OBJS) -o $@

# --- Compile ------------------------------------------------------------

$(RELEASE_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARNINGS) $(INCLUDES) $(RELEASE_FLAGS) -c $< -o $@

$(DEBUG_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXSTD) $(WARNINGS) $(INCLUDES) $(DEBUG_FLAGS) -c $< -o $@

# --- Convenience targets --------------------------------------------------

run: release
	./$(RELEASE_DIR)/$(TARGET)

run-debug: debug
	./$(DEBUG_DIR)/$(TARGET)

clean:
	rm -rf build
