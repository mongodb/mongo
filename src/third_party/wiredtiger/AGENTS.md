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

## Writing Tests

### Determinism

Most flaky tests in this repo come from a short list of causes. Check a new test against all of them.

- **Never assert on state a background thread owns without waiting for it.** Eviction, checkpoint, sweep, garbage collection, and the disaggregated-storage pickup server all run asynchronously; a state change is not visible just because the call that triggered it returned. Poll for the state with a deadline and assert the deadline inside the loop, so a timeout fails as a timeout — see `test_layered_schema25.py` for the pattern.
- **Do not use a sleep to wait for a state.** A sleep long enough to pass on an idle machine is still too short on an ASan or heavily loaded variant. Sleeping to *advance* time, or to let a thread make progress with no state to poll, is fine.
- **Assert direction, not exact values, on statistics.** Cache and reconciliation counters move with eviction timing and page splits. Prefer non-zero, monotonic, or bounded-range checks unless the counter is genuinely deterministic.
- **Seed randomness and record the seed** in the failure output, so a failure can be reproduced.
- **Do not tune data volumes to barely trigger the condition.** A margin that just works locally disappears on slower variants; size the workload so the condition is reached comfortably.
- **Do not depend on state another test leaves behind**, including timestamps, cached files, and connection configuration.

### Verifying a New Test

A single green run is not evidence. Before submitting:

1. **Confirm the test fails without the fix.** A test that cannot fail is worse than no test.
2. **Run it repeatedly** — `for i in $(seq 100); do python3 ../test/suite/run.py <test> || break; done` — for anything touching background threads.

### Style

- Describe behaviour through the public API. Do not explain internal mechanics in test comments.
- Do not reference Jira tickets in test code; name the scenario by what it exercises.
- Parameterize over scenarios rather than copying a test body for trivially different data.

## CI

Runs on MongoDB Evergreen. Config: `test/evergreen.yml` (and `test/evergreen_disagg.yml` for disaggregated storage).
