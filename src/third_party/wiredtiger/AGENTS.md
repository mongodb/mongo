## Project Overview

WiredTiger is a high-performance, embedded key-value storage engine written in C. It is the default storage engine for MongoDB. Tests and tooling use C, C++, and Python.

## Build & Test

```bash
# Configure (Ninja recommended)
cmake -B build -G Ninja

# Useful CMake options:
#   -DHAVE_DIAGNOSTIC=1   diagnostic checks (default for non-Release)
#   -DHAVE_UNITTEST=1     Catch2 unit tests
#   -DENABLE_PYTHON=1     Python API
#   -DCMAKE_BUILD_TYPE=   Release | Debug | ASan | TSan | UBSan | MSan

cmake --build build                               # build
ctest --test-dir build -j$(nproc)                 # all C/C++ tests
ctest --test-dir build -R <regex> -j$(nproc)      # subset by name

# Catch2 (run from build/, requires -DHAVE_UNITTEST=1)
./test/catch2/catch2-unittests              # all
./test/catch2/catch2-unittests "[tag]"      # one subsystem tag

# Python suite (run from build/, requires -DENABLE_PYTHON=1)
python3 ../test/suite/run.py                # all
python3 ../test/suite/run.py <test_name>    # single test
```

## Formatting & Validation

Run from the repo root before submitting:

```bash
cd dist && ./s_all     # full validation + clang-format
cd dist && ./s_fast    # fast subset, only changed files
```

`s_all` also regenerates code from data definitions in `dist/` (see below). Re-run it after editing any `dist/*.py` data file.

## Code Architecture

### Source layout (`src/`)

- **Data path**: `btree/`, `cursor/`, `block/`, `reconcile/`
- **Transactions & durability**: `txn/`, `checkpoint/`, `log/`, `rollback_to_stable/`, `history/`
- **Connection & session**: `conn/`, `session/`, `schema/`
- **Memory**: `cache/`, `evict/`
- **Storage extensions**: `block_cache/`, `block_disagg/`, `live_restore/`, `tiered/`
- **Platform**: `os_posix/`, `os_win/`, `os_common/`, `os_darwin/`, `os_linux/`
- **Infrastructure**: `config/`, `support/`, `meta/`, `packing/`, `checksum/`
- **Headers**: `include/` — `wiredtiger.h.in` is the public API template; `wt_internal.h` aggregates internal headers
- **CLI**: `utilities/` — the `wt` command-line tool

### Public API handles

- `WT_CONNECTION` — database connection (typically one per process)
- `WT_SESSION` — operational context (one per thread)
- `WT_CURSOR` — key-value iterator (owned by a session)

### Generated code (`dist/`)

Python scripts generate C code from data definitions. Edit the data file, then run `dist/s_all`:

- `api_data.py` → config parsing
- `stat_data.py` → statistics
- `log_data.py` → log records
- `flags.py` → flag values
- `prototypes.py` → function prototypes

### Examples

Working code samples live in `examples/c/` and `examples/python/`.

## C Coding Conventions

Full rules in @CONTRIBUTING.md.

### Comment Prose Style

Mechanics — delimiter style, function-header layout, FIXME tags, wrap width — live in @CONTRIBUTING.md and are enforced by `dist/s_style`, `dist/comment_style.py`, and `dist/s_comment.py`. The guidance here is about what to write in the prose, not how to punctuate it.

- **Be terse.** Aim for one sentence. If you are writing three, you are probably restating the code, explaining things the reader already knows, or narrating the editing session. The codebase prefers no comment to a wordy one — most lines have no comment at all.
- **Write for a working WiredTiger engineer.** Do not explain concepts a developer in this codebase already understands — hazard pointers, reconciliation, the history store, eviction, dhandles, session and cursor semantics, btree splits, transactions, timestamps.
- **Reserve block comments for *why*, not *what*.** Good targets:
  - Concurrency invariants and the reason for an ordering, barrier, or lock.
  - Counter-intuitive control flow, loop direction, or termination condition.
  - Performance constraints, on-disk format constraints, or block-manager limits.
  - References to the algorithm, data structure, or paper being implemented.
  - Cross-references to other functions whose contract this code depends on.
- **Do not block-comment routine code:** variable declarations, simple assignments, standard `WT_RET()` / `WT_ERR()` chains, obvious branches.
- **Describe roles, not identifiers.** Write `the cursor` or `the page being evicted`, not `cbt` or `ref->page`.
- **Anchor in the codebase, not the editing session.** A comment must read sensibly to someone who only sees the final code. Do not reference the Jira ticket, PR, branch, or author behind the change — that belongs in the commit message. Do not reference closed tickets or merged PRs. Above all, do not reference work that only ever existed in the editing session — earlier iterations, an approach that was reverted, an experiment that never landed.
- **Prefer `FIXME-WT-XXXX` over `TODO` and `XXX` in new code.** Legacy `TODO` / `XXX` markers exist but are not the preferred voice; file a ticket and reference it.
- **No decorative material.** Skip banners, ASCII separators, and section dividers inside functions. Skip `added by` / `modified for X` / `see ticket Y` provenance notes.

## Test Frameworks

| Framework | Location | Purpose |
|-----------|----------|---------|
| Catch2 | `test/catch2/` | C++ unit tests below the API (needs `-DHAVE_UNITTEST=1`) |
| Python suite | `test/suite/` | Functional/integration via Python API |
| csuite | `test/csuite/` | C-based sanity and integration |
| format | `test/format/` | Randomized stress/fuzz |
| cppsuite | `test/cppsuite/` | C++ stress framework |
| checkpoint | `test/checkpoint/` | Checkpoint stress |
| model | `test/model/` | Lightweight formal verification |
| wtperf | `bench/wtperf/` | Performance benchmarks |

## CI

Runs on MongoDB Evergreen. Config: `test/evergreen.yml` (and `test/evergreen_disagg.yml` for disaggregated storage).
