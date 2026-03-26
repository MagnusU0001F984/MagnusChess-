CXX := g++
TARGET := valerain

BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj
BIN_DIR := bin
PGO_DIR := $(BUILD_DIR)/pgo

SRC := \
	Attack.cpp \
	Bench.cpp \
	Evaluation.cpp \
	History.cpp \
	Main.cpp \
	MoveGen.cpp \
	Nnue.cpp \
	Perft.cpp \
	Position.cpp \
	Search.cpp \
	See.cpp \
	Time.cpp \
	TT.cpp \
	Uci.cpp

OBJ := $(patsubst %.cpp,$(OBJ_DIR)/%.o,$(SRC))
DEP := $(OBJ:.o=.d)

ARCH_FLAGS := \
	-m64 \
	-mavx2 \
	-mbmi2 \
	-mpopcnt \
	-mlzcnt

OPT_FLAGS := \
	-Ofast \
	-DNDEBUG \
	-flto=auto \
	-fuse-linker-plugin \
	-fomit-frame-pointer \
	-funroll-loops \
	-fstrict-aliasing \
	-ffast-math \
	-fno-math-errno \
	-fno-signed-zeros \
	-fno-trapping-math \
	-fdevirtualize \
	-fdevirtualize-at-ltrans \
	-ftree-vectorize \
	-fipa-pta \
	-fipa-cp \
	-fipa-cp-clone \
	-fipa-icf \
	-falign-functions=32 \
	-falign-loops=32 \
	-falign-jumps=32 \
	-falign-labels=32 \
	-fweb \
	-fprefetch-loop-arrays

LOWLEVEL_FLAGS := \
	-fno-exceptions \
	-fno-rtti

WARN_FLAGS := \
	-Wall \
	-Wextra \
	-Wshadow \
	-Wconversion \
	-Wcast-qual \
	-Wno-unused-parameter \
	-Wno-sign-conversion

CPPFLAGS := -MMD -MP

CXXFLAGS := \
	-std=c++20 \
	$(ARCH_FLAGS) \
	$(OPT_FLAGS) \
	$(LOWLEVEL_FLAGS) \
	$(WARN_FLAGS) \
	$(CPPFLAGS)

LDFLAGS := \
	$(ARCH_FLAGS) \
	-Ofast \
	-flto=auto

LDLIBS :=

.PHONY: all release releases
all: $(BIN_DIR)/$(TARGET).exe
release: all
releases: all

$(BIN_DIR)/$(TARGET).exe: $(OBJ) | $(BIN_DIR)
	$(CXX) $(OBJ) -o $@ $(LDFLAGS) $(LDLIBS)

$(OBJ_DIR)/%.o: %.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJ_DIR):
	@if not exist "$(OBJ_DIR)" mkdir "$(OBJ_DIR)"

$(BIN_DIR):
	@if not exist "$(BIN_DIR)" mkdir "$(BIN_DIR)"

$(PGO_DIR):
	@if not exist "$(PGO_DIR)" mkdir "$(PGO_DIR)"

.PHONY: pgo-generate
pgo-generate: CXXFLAGS += -fprofile-generate=$(PGO_DIR)
pgo-generate: LDFLAGS  += -fprofile-generate=$(PGO_DIR)
pgo-generate: clean $(BIN_DIR)/$(TARGET)_pgo.exe

$(BIN_DIR)/$(TARGET)_pgo.exe: $(OBJ) | $(BIN_DIR)
	$(CXX) $(OBJ) -o $@ $(LDFLAGS) $(LDLIBS)

.PHONY: pgo-use
pgo-use: CXXFLAGS += -fprofile-use=$(PGO_DIR) -fprofile-correction
pgo-use: LDFLAGS  += -fprofile-use=$(PGO_DIR) -fprofile-correction
pgo-use: clean $(BIN_DIR)/$(TARGET).exe

.PHONY: run
run: all
	.\$(BIN_DIR)\$(TARGET).exe

.PHONY: clean
clean:
	@if exist "$(OBJ_DIR)" rmdir /s /q "$(OBJ_DIR)"
	@if exist "$(BIN_DIR)\$(TARGET).exe" del /q "$(BIN_DIR)\$(TARGET).exe"
	@if exist "$(BIN_DIR)\$(TARGET)_pgo.exe" del /q "$(BIN_DIR)\$(TARGET)_pgo.exe"

.PHONY: distclean
distclean: clean
	@if exist "$(PGO_DIR)" rmdir /s /q "$(PGO_DIR)"
	@if exist "$(BUILD_DIR)" rmdir /s /q "$(BUILD_DIR)"

-include $(DEP)
