# Magnus

Magnus is a C++20 chess engine with UCI support, a classical search core, optional NNUE evaluation, and built-in bench/perft/divide tooling for validation and performance checks.

## Technical Overview

### 1. Core Architecture
- Language/standard: `C++20`
- Board model: bitboards with incremental state maintenance (hash key, eval cache, king-square cache, etc.)
- Main modules: `MoveGen / Search / TT / History / Evaluation / NNUE / UCI / Time`

### 2. Search Framework
- Iterative Deepening
- Main search: PVS (Principal Variation Search)
- Tactical frontier: Quiescence Search
- Aspiration Windows (with fallback widening)
- Full `seldepth` tracking in UCI `info` output

### 3. Move Ordering and Learned Signals
- Staged MovePicker in main search:
  - `TT move -> good captures -> killer1/2 -> quiets -> bad captures`
- Quiet ordering signals:
  - quiet history
  - countermove
  - continuation (prev / prev2)
- Capture ordering signals:
  - capture history
  - immediate SEE term
  - see-bias term
  - MVV-LVA support term

### 4. Implemented Pruning/Reductions (Main Search)
- TT cutoffs (bound-aware)
- IIR (Internal Iterative Reduction)
- Razoring
- Reverse Futility
- Null Move Pruning
- SEE late bad-capture gate
- Futility pruning (shallow quiets)
- LMP (Late Move Pruning)
- History pruning
- Partial LMR:
  - quiet LMR (quiet strength + improving signal)
  - capture LMR (simple-capture branch + fail-high re-search)

### 5. History / Experience System
- killer table
- quiet history table
- capture history table
- countermove table
- continuation history table
- see-bias table
- reward/penalty paths for cutoff winners and failed alternatives (including continuation and see-bias updates)

### 6. Transposition Table (TT)
- 64-byte clusters, 4 lanes
- probe/save + prefetch
- replacement policy aware of depth/age/bound/PV
- sampled `hashfull` reporting (with reduced reporting frequency)

### 7. Evaluation
- HCE: tapered eval (MG/EG interpolation) + PST, incrementally maintained
- NNUE: optional (`UseNNUE` + `EvalFile`), automatic fallback to HCE if unavailable

### 8. Time Management and UCI
- Standard UCI commands:
  - `uci / isready / position / go / stop / quit / setoption`
- Exposed UCI options:
  - `Hash / Threads / Contempt / Clear Hash / UseNNUE / Ponder / EvalFile`
- `go` supports:
  - depth, nodes, movetime, wtime/btime/inc/movestogo
- TimeManager adapts time usage with recent-search history

---

## Build

### Requirements
- Compiler: `g++` with `-std=c++20`
- Build tool: `make` (`mingw32-make` on Windows)
- Default target CPU flags in `src/Makefile`:
  - `AVX2 + BMI2 + POPCNT + LZCNT`
- If your CPU does not support these, adjust `ARCH_FLAGS` in `src/Makefile`.

### Directory Layout
- Source: `src/`
- Binaries: `src/bin/`
- Build artifacts: `src/build/`

### Windows (PowerShell + MinGW)
```powershell
cd src
mingw32-make clean
mingw32-make -j
```

Output:
- `src/bin/magnus.exe`

### Linux / WSL / MSYS2
```bash
cd src
make clean
make -j
```

Output:
- `src/bin/magnus`

### Optional PGO Flow
1. Build an instrumented binary:
```bash
cd src
make pgo-generate
```
2. Run representative workloads to generate profile data:
```bash
./bin/magnus_pgo search 20 64
```
3. Rebuild with profile use:
```bash
make pgo-use
```

---

## Run Examples

### UCI Mode
```bash
# No arguments starts UCI mode
./src/bin/magnus

# Equivalent explicit form
./src/bin/magnus uci
```

### Bench / Perft / Divide / Fixed-Depth Search
```bash
# perft: <depth> <hash_mb> <threads>
./src/bin/magnus 6 64 1

# divide: divide <depth> <hash_mb> <threads> [live]
./src/bin/magnus divide 6 64 1

# search: search <depth> <hash_mb>
./src/bin/magnus search 8 64
```
