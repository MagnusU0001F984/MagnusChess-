CXX := g++
TARGET := valerain

BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj
BIN_DIR := bin
PGO_DIR := $(BUILD_DIR)/pgo
PGO_BENCH_LOG := $(PGO_DIR)/bench.log
PGO_BENCH_ARGS ?= bench

SRC := \
	Attack.cpp \
	Bench.cpp \
	Evaluation.cpp \
	History.cpp \
	Lmr.cpp \
	Main.cpp \
	MovePicker.cpp \
	MoveGen.cpp \
	Nmp.cpp \
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
	-mlzcnt \
	-mtune=native

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
	-fipa-ra \
	-falign-functions=32 \
	-falign-loops=32 \
	-falign-jumps=32 \
	-falign-labels=32 \
	-fweb \
	-fprefetch-loop-arrays \
	-frename-registers

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

BASE_CXXFLAGS := \
	-std=c++20 \
	$(ARCH_FLAGS) \
	$(OPT_FLAGS) \
	$(LOWLEVEL_FLAGS) \
	$(WARN_FLAGS) \
	$(CPPFLAGS)

LINK_OPT_LEVEL ?= 3

BASE_LDFLAGS := \
	$(ARCH_FLAGS) \
	-Ofast \
	-flto=auto \
	-Wl,-O$(LINK_OPT_LEVEL)

PGO_GENERATE_CXXFLAGS := -fprofile-generate=$(PGO_DIR)
PGO_GENERATE_LDFLAGS := -fprofile-generate=$(PGO_DIR)
PGO_USE_CXXFLAGS := \
	-fprofile-use=$(PGO_DIR) \
	-fprofile-correction \
	-fno-peel-loops \
	-fno-tracer

LDLIBS :=

ifeq ($(OS),Windows_NT)
	EXEEXT := .exe

	fixpath = $(subst /,\,$(1))

	MKDIR_P = if not exist "$(call fixpath,$(1))" mkdir "$(call fixpath,$(1))"
	RM_F    = if exist "$(call fixpath,$(1))" del /q "$(call fixpath,$(1))"
	RMDIR_R = if exist "$(call fixpath,$(1))" rmdir /s /q "$(call fixpath,$(1))"
else
	EXEEXT :=
	MKDIR_P = mkdir -p "$(1)"
	RM_F    = rm -f "$(1)"
	RMDIR_R = rm -rf "$(1)"
endif

OUTPUT := $(BIN_DIR)/$(TARGET)$(EXEEXT)
PROFGEN_OUTPUT := $(BIN_DIR)/$(TARGET)_profgen$(EXEEXT)
PGO_OUTPUT := $(BIN_DIR)/$(TARGET)_pgo$(EXEEXT)

ifeq ($(OS),Windows_NT)
	RUN_CMD = .\$(call fixpath,$(OUTPUT))
	RUN_PROFGEN_CMD = .\$(call fixpath,$(PROFGEN_OUTPUT))
	RUN_PGO_CMD = .\$(call fixpath,$(PGO_OUTPUT))
	PGO_BENCH_LOG_CMD = $(call fixpath,$(PGO_BENCH_LOG))
else
	RUN_CMD = ./$(OUTPUT)
	RUN_PROFGEN_CMD = ./$(PROFGEN_OUTPUT)
	RUN_PGO_CMD = ./$(PGO_OUTPUT)
	PGO_BENCH_LOG_CMD = $(PGO_BENCH_LOG)
endif

.PHONY: all build release releases \
	pgo profile-build pgo-step1 pgo-step2 pgo-step3 \
	pgo-generate-build pgo-use-build run clean objclean pgo-clean distclean

all: build
build: $(OUTPUT)
release: build
releases: build

$(OUTPUT): $(OBJ) | $(BIN_DIR)
	$(CXX) $(OBJ) -o $@ $(BASE_LDFLAGS) $(PGO_LDFLAGS) $(LDLIBS)

$(PROFGEN_OUTPUT): $(OBJ) | $(BIN_DIR)
	$(CXX) $(OBJ) -o $@ $(BASE_LDFLAGS) $(PGO_LDFLAGS) $(LDLIBS)

$(PGO_OUTPUT): $(OBJ) | $(BIN_DIR)
	$(CXX) $(OBJ) -o $@ $(BASE_LDFLAGS) $(PGO_LDFLAGS) $(LDLIBS)

$(OBJ_DIR)/%.o: %.cpp | $(OBJ_DIR)
	$(CXX) $(BASE_CXXFLAGS) $(PGO_CXXFLAGS) -c $< -o $@

$(OBJ_DIR):
	$(call MKDIR_P,$@)

$(BIN_DIR):
	$(call MKDIR_P,$@)

$(PGO_DIR):
	$(call MKDIR_P,$@)

pgo-generate-build: PGO_CXXFLAGS = $(PGO_GENERATE_CXXFLAGS)
pgo-generate-build: PGO_LDFLAGS = $(PGO_GENERATE_LDFLAGS)
pgo-generate-build: $(PROFGEN_OUTPUT)

pgo-use-build: PGO_CXXFLAGS = $(PGO_USE_CXXFLAGS)
pgo-use-build: $(PGO_OUTPUT)

pgo: profile-build

profile-build:
	@echo Step 1/3. Building instrumented executable ($(PROFGEN_OUTPUT)) ...
	@$(MAKE) --no-print-directory pgo-step1
	@echo Step 2/3. Running bench to collect profile data ...
	@$(MAKE) --no-print-directory pgo-step2
	@echo Step 3/3. Rebuilding optimized executable with PGO ($(PGO_OUTPUT)) ...
	@$(MAKE) --no-print-directory pgo-step3

pgo-step1: objclean pgo-clean
	$(call RM_F,$(PROFGEN_OUTPUT))
	$(call RM_F,$(PGO_OUTPUT))
	@$(MAKE) --no-print-directory pgo-generate-build

pgo-step2: | $(PGO_DIR)
	@$(MAKE) --no-print-directory pgo-generate-build
	$(RUN_PROFGEN_CMD) $(PGO_BENCH_ARGS) > "$(PGO_BENCH_LOG_CMD)" 2>&1
	@echo PGO bench log written to $(PGO_BENCH_LOG)

pgo-step3:
	@$(MAKE) --no-print-directory objclean
	$(call RM_F,$(PGO_OUTPUT))
	@$(MAKE) --no-print-directory pgo-use-build

.PHONY: run
run: all
	$(RUN_CMD)

objclean:
	$(call RMDIR_R,$(OBJ_DIR))

pgo-clean:
	$(call RMDIR_R,$(PGO_DIR))

clean:
	$(call RMDIR_R,$(OBJ_DIR))
	$(call RM_F,$(OUTPUT))
	$(call RM_F,$(PROFGEN_OUTPUT))
	$(call RM_F,$(PGO_OUTPUT))

distclean: clean
	$(call RMDIR_R,$(PGO_DIR))
	$(call RMDIR_R,$(BUILD_DIR))

-include $(DEP)
