---
name: resmoke
description: Run MongoDB integration and correctness tests using the resmoke test orchestrator. Use when working with jstests, test suites, or running MongoDB integration tests.
---

# Resmoke Test Orchestrator

Resmoke is MongoDB's integration testing framework for running JavaScript tests, C++ integration tests, and multiversion tests. Resmoke manages its own clean test environment; no pre-cleanup of data directories is needed.

## Basic Usage

Activate the virtual environment before running resmoke:

```bash
source python3-venv/bin/activate
```

### Running Tests

Run a test suite:

```bash
python3 buildscripts/resmoke.py run --suites=core
```

Run multiple suites:

```bash
python3 buildscripts/resmoke.py run --suites=core,no_passthrough
```

Run a single test file:

```bash
python3 buildscripts/resmoke.py run --suites=core jstests/core/administrative/auth1.js
```

Run multiple test files:

```bash
python3 buildscripts/resmoke.py run --suites=core jstests/core/administrative/auth1.js jstests/core/administrative/auth2.js
```

Run tests matching a pattern:

```bash
python3 buildscripts/resmoke.py run --suites=core jstests/core/administrative/*.js
```

Dry run to see what tests would execute:

```bash
python3 buildscripts/resmoke.py run --suites=core -n
```

### Common Test Suites

| Suite            | Purpose                                      |
| ---------------- | -------------------------------------------- |
| `core`           | General mongod tests (runs as standalone)    |
| `no_passthrough` | Tests that manage their own mongod instances |
| `auth`           | Authentication/authorization tests           |
| `sharding`       | Sharded cluster tests                        |
| `replica_sets`   | Replication tests                            |
| `concurrency`    | Concurrency and locking tests                |

## Exploring Tests

List available suites:

```bash
python3 buildscripts/resmoke.py list-suites
```

Discover tests in a suite:

```bash
python3 buildscripts/resmoke.py test-discovery --suite=core
```

View suite configuration:

```bash
python3 buildscripts/resmoke.py suiteconfig --suite=core
```

List available tags:

```bash
python3 buildscripts/resmoke.py list-tags
```

## Output Format and Parallel Execution

### Output Structure

Resmoke logs are prefixed with component tags:

```
[resmoke] 2026-03-02T12:59:22.431Z Starting test execution...
[job0] 2026-03-02T12:59:23.123Z Running jstests/core/administrative/auth1.js...
[job1] 2026-03-02T12:59:23.145Z Running jstests/core/administrative/auth2.js...
```

Useful patterns to grep for:

- `\[resmoke\]` - Main orchestrator messages
- `\[job\d+\]` - Individual job execution logs
- `FAILED` - Test failures
- `PASSED` - Successful test completion
- `Running` - Currently executing test
- `Starting.*test|test.*complete` - Test execution lifecycle

### Parallel Execution

By default, resmoke runs tests sequentially. Use `-j` for parallel execution:

```bash
python3 buildscripts/resmoke.py run --suites=core -j4
python3 buildscripts/resmoke.py run --suites=core -j$(nproc)
```

Each parallel job:

- Gets a unique `jobN` prefix in logs for identification
- Has isolated port ranges for fixture processes
- Maintains separate data directories

When debugging failures in parallel runs, use `--continueOnFailure` to run all tests before reporting:

```bash
python3 buildscripts/resmoke.py run --suites=core -j4 --continueOnFailure
```

> [!NOTE]
> Full suites can take well over 10 minutes. For long runs, use `--alwaysUseLogFiles`; the per-job log files can be read or searched incrementally while the suite is running.

## Common Options

| Option                  | Description                                                                                                            |
| ----------------------- | ---------------------------------------------------------------------------------------------------------------------- |
| `-n` / `--dryRun=tests` | Dry run mode (shows tests without running)                                                                             |
| `-j JOBS`               | Parallel job count                                                                                                     |
| `--continueOnFailure`   | Continue after test failures                                                                                           |
| `--testTimeout SECONDS` | Timeout per test                                                                                                       |
| `--logLevel DEBUG`      | Verbose logging                                                                                                        |
| `--alwaysUseLogFiles`   | Write per-job log files (`resmoke_job0.log`, etc.) to the working directory; useful for monitoring long-running suites |

### Additional options

You can always run `resmoke -h` to discover all available options. It can also be passed to subcommands like `resmoke run -h`. If you discover something generically helpful that isn't listed anywhere in this skill yet, you should add it to one of the other files so other agents can benefit.

## Debugging Failed Tests

### Hang Analyzer

Analyze hung processes:

```bash
python3 buildscripts/resmoke.py hang-analyzer -p mongod,mongos
```

### Core Dump Analysis

Analyze core dumps:

```bash
python3 buildscripts/resmoke.py core-analyzer -c core.dump
```

## Advanced Topics

For detailed information on:

- **Advanced options & multiversion testing**: See [advanced-usage.md](advanced-usage.md)
