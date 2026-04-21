# QueryTester

## Overview

**QueryTester** is a test harness designed to streamline E2E logic testing of MongoDB queries. It
validates query results by executing them against a live MongoDB instance (e.g. `mongod`, `mongos`,
or any system that implements the MongoDB wire protocol) and comparing the output to pre-defined
expected results.

QueryTester is ideal for small, reproducible test cases that verify query behavior with minimal
setup. This tool follows a focused paradigm, exclusively supporting queries and DML operations with
configurable settings. Support for passthroughs may be added in the future.

QueryTester does not currently support extensive setup or complex infrastructure, but it is designed
to be extensible, with the potential to handle more complex environments in the future. The overall
goal of Tester's design, however, is to validate query logic in a simple, clear, and consistent
manner.

Each QueryTester use case expects three files to work together: a `.test`, a `.results`, and a
`.coll`. See [below](#file-types-and-formats) for templates of each.

## Debugging BFs

See
[Runbook: Triaging Query Correctness BFs](https://docs.google.com/document/d/1lIdwnR_pMoYEBKL8Np8X4igMqIUultegy8As_9g2R8Y/edit?tab=t.0#heading=h.13k8s02tb8j3)

## Getting Started

You can compile the tester using the following command:

```sh
bazel build install-mongotest
```

## Running Tests

The tester expects a mongod/mongos to be running, and will execute tests against that process.

To run a single test for the first time, try using the following command from the root of the mongo
repo:

```sh
mongotest -t <QueryTesterDir>/tests/manual_tests/example/testA.test --drop --load --mode compare
```

This will run `testA` and verify the results are correct, assuming that `testA.results` exists.

To perform other operations, consult the table below.

### Options

| Option                             | Description                                                                                                                                                                                                                                                                                            |
| ---------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `-t <path/to/test>`                | Path to a `.test` file to run. Required. Can be specified multiple times to run several test files in sequence.                                                                                                                                                                                        |
| `--uri <MDBConnString>`            | MongoDB connection URI. Defaults to `mongodb://localhost:27017` if not specified. Not used in `--mode normalize`.                                                                                                                                                                                      |
| `-n <int>`                         | Run only the test at position `<int>` (0-indexed) in the immediately preceding `-t` file. For example, `-t foo.test -n 2` runs the third test in `foo.test`. Must immediately follow a `-t` argument. Incompatible with `--out`.                                                                       |
| `-r <start> <end>`                 | Run only tests numbered `<start>` through `<end>` (inclusive) from the immediately preceding `-t` file. `<start>` must be ≤ `<end>`. Must immediately follow a `-t` argument.                                                                                                                          |
| `-v (verbose)`                     | Print a summary of failing queries after an unsuccessful run. Requires `--mode compare`. Combine with `--extractFeatures` for richer per-failure diagnostics.                                                                                                                                          |
| `--extractFeatures`                | For each failed query, extract syntax, query planner, and execution stats metadata to aid debugging. Requires `-v` and `--mode compare`. The [feature-extractor](https://github.com/10gen/feature-extractor) tool must be present in the user's home directory.                                        |
| `--drop`                           | Drop the test collections before loading them. Almost always paired with `--load`; without it the collections will be empty after the drop.                                                                                                                                                            |
| `--load`                           | Insert documents and build indexes for all collections referenced by the test files. If omitted, the collections are assumed to already contain the correct data.                                                                                                                                      |
| `--minimal-index`                  | Skip non-essential index creation and only build indexes required for queries to run at all (currently: geospatial and text indexes). Speeds up collection loading at the cost of missing index coverage.                                                                                              |
| `--mode [run, compare, normalize]` | `compare` (default): run each test and fail if results differ from the `.results` file. `run`: execute tests and optionally write output via `--out` — tests only fail on execution errors. `normalize`: validate that existing `.results` files are in canonical form without connecting to a server. |
| `--opt-off`                        | Disable query optimizations and find-layer pushdown. Used to generate a baseline results file for differential/multiversion testing. Requires `--enableTestCommands=true` on the mongod.                                                                                                               |
| `--out [result, oneline]`          | Write results to a `.results` file after running. `result` formats each document on its own line; `oneline` puts the entire result set on one line. Overwrites existing `.results` files. Not available with `--mode compare` or when using `-n`/`-r`.                                                 |
| `--populateAndExit`                | Drop and reload collection data, then exit without running any tests. Implicitly applies `--drop` and `--load`. Accepts exactly one `-t` argument.                                                                                                                                                     |
| `--diff [plain, word]`             | Controls how result differences are displayed. `word` (default) shows colored word-level diffs — recommended for ANSI-capable terminals. `plain` shows uncolored line-level diffs.                                                                                                                     |
| `--override [queryShapeHash]`      | Override the test type for the run. Currently the only supported value is `queryShapeHash`, which runs explain on each query and asserts that the extracted query shape hash matches the expected value in a `.queryShapeHash.results` file.                                                           |
| `--ignore-index-failures`          | Suppress errors from index creation failures and continue loading. Useful when running against a server version that does not support all index types in the test collection.                                                                                                                          |

## File types and formats

### .test

See `tests/manual_tests/example/testA.test`. The file format is as follows: First line must be the
testName (matching the filename without the extension). Second line is the database to run the test
against. Third line is a list of collection files to load. All collections are expected to be in a
`collections` directory somewhere along the path to the test file. Fourth line is a newline noting
the end of the header. After the header, each line is a test line: `<testType> {commandToRun}` with
each test line being followed by a newline.

The template is as follows:

```
<testName>
<database>
<collection file (*.coll)>

<testType> <{commandToRun}>
... further tests
```

#### Test Types

| Type                          | Description                                                                                                                                                                                                              |
| ----------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| :results                      | The result array specified in the `.results` file will be compared exactly with the document results from running the command. Order will be considered.                                                                 |
| :sortResults                  | Same as above, but order will not be considered. All documents in each set must appear in the other.                                                                                                                     |
| :sortBSON                     | Same as above, but also sorts the fields in each BSONObj in the result set. All documents in each set must appear in the other.                                                                                          |
| :sortFull                     | Same as above, but also sorts arrays within each BSONObj in the result set. All documents in each set must appear in the other. This is the setting the fuzzer currently uses for results comparison.                    |
| :normalizeNumerics            | All numeric elements in the input array will be normalized to Decimal128 so that equivalent values will be considered equal regardless of the original numeric type. All documents in each set must appear in the other. |
| :sortResultsNormalizeNumerics | Combination of :sortResults and :normalizeNumerics. All documents in each set must appear in the other.                                                                                                                  |
| :sortBSONNormalizeNumerics    | Combination of :sortBSON and :normalizeNumerics. All documents in each set must appear in the other.                                                                                                                     |
| :normalizeNulls               | Null, undefined, and missing will be considered the same. All documents in each set must appear in the other.                                                                                                            |
| :normalizeNonNull             | Combination of :sortFull and :normalizeNumerics. All documents in each set must appear in the other.                                                                                                                     |
| :normalizeFull                | Combination of :sortFull, :normalizeNumerics, and :normalizeNulls. All documents in each set must appear in the other. This is the widest normalization setting.                                                         |
| :queryShapeHash               | Runs the given query as an explain command, extracts the queryShapeHash and asserts that it is the same as the as expected.                                                                                              |
| :explain                      | Not yet implemented. We will eventually support some form of the explain command.                                                                                                                                        |

### .results

See `tests/manual_tests/example/testA.results`. These have the same format as .test files above,
with the exception that each test must be followed by a line with the expected documents. These are
allowed to be on multiple lines, and a result array is read as the line after the test until the
next newline.

The template is as follows:

```
<testName>
<database>
<collection file (*.coll)>

<testType> <{commandToRun}>
[
    <result 0>,
    <result 1>,
    ... further results
]
... further tests and results
```

Some files have a `.queryShapeHash.results` extension. These are the expected results for the
queryShapeHash test type, and they only contain the queryShapeHash, as in the
`tests/manual_tests/example/testQueryShapeHash.queryShapeHash.results` example.

### .coll

See `tests/manual_tests/example/basic.coll`. These files are split into two sections divided by an
empty line. Above the empty line are index definitions, one per line. They can be of the form:

1. `{<index>}`, or
2. `{key: <index>}`, or
3. `{key: <index>, options: <indexOptions>}`

Below the empty line are documents, one per line.

These files are referenced by test files, and can/should be shared across tests.

The template is as follows:

```
<{index}>
<{index}>
... further indexes

<{document}>
<{document}>
... further documents
```

### Comments

Whole-line inline comments can be added in any `.test`, `.results`, and `.coll` file by starting the
line with `//`. Partial-line comments, such as `foo // comment`, are not supported, and the line
will be read in its entirety.

Comments in input `.test` and `.results` files will be persisted in the output as much as possible.

For files containing result set output, comments interleaved between result set lines will be
**ignored** and absent from the output file (see `normalizedCommentsTest.pre` and
`normalizedCommentsTest.results` for an example).

---

[Return to Cover Page](../README_QO.md)
