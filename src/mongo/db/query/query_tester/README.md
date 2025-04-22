# QueryTester

## Overview

**QueryTester** is a test harness designed to streamline E2E logic testing of MongoDB queries. It validates query results by executing them against a live MongoDB instance (e.g. `mongod`, `mongos`, or any system that implements the MongoDB wire protocol) and comparing the output to pre-defined expected results.

QueryTester is ideal for small, reproducible test cases that verify query behavior with minimal setup. This tool follows a focused paradigm, exclusively supporting queries and DML operations with configurable settings. Support for passthroughs may be added in the future.

QueryTester does not currently support extensive setup or complex infrastructure, but it is designed to be extensible, with the potential to handle more complex environments in the future. The overall goal of Tester's design, however, is to validate query logic in a simple, clear, and consistent manner.

Each QueryTester use case expects three files to work together: a `.test`, a `.results`, and a `.coll`. See [below](#file-types-and-formats) for templates of each.

## Getting Started

You can compile the tester using the following command:

```sh
bazel build install-mongotest
```

## Running Tests

The tester expects a mongod/mongos to be running, and will execute tests against that process.

To run a single test for the first time, try using the following command from the root of the mongo repo:

```sh
mongotest -t <QueryTesterDir>/tests/manual_tests/example/testA.test --drop --load --mode compare
```

This will run `testA` and verify the results are correct, assuming that `testA.results` exists.

To perform other operations, consult the table below.

### Options

| Option                           | Description                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           |
| -------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| -t <path/to/testName>            | Required. Can appear multiple times, each time it appears the test following will be run. All test files should use the `.test` suffix.                                                                                                                                                                                                                                                                                                                                                               |
| --uri <MDBConnString>            | The address at which to connect to a mongod/mongos. Defaults to localhost::27017. Uses the MongoDB URI format                                                                                                                                                                                                                                                                                                                                                                                         |
| -n <int>                         | Run a specific test in the file immediately preceding this -n argument. Invalid if not following a -t <testName> pair                                                                                                                                                                                                                                                                                                                                                                                 |
| -r <int> <int>                   | Run a range of tests in the file immediately preceding this -r argument. Invalid if not following a -t <testName> pair                                                                                                                                                                                                                                                                                                                                                                                |
| -v (verbose)                     | Only available in compare mode. This appends a summary of failing queries to an unsuccessful test file comparison.                                                                                                                                                                                                                                                                                                                                                                                    |
| --extractFeatures                | Only available in compare mode, and must be specified with -v (verbose). Extracts metadata about most common features across failed queries for an enriched debugging experience. Note that this uses the [feature-extractor](https://github.com/10gen/feature-extractor), which must be present in the user's home directory.                                                                                                                                                                        |
| --drop                           | Drops the collections needed by the tests to be run before running.                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| --load                           | Builds indexes and inserts documents into the collections needed by the specified test files. If not specified assumes the collection state is correct                                                                                                                                                                                                                                                                                                                                                |
| --minimal-index                  | Only create the minimal set of indices necessary, currently just geospatial and text indices.                                                                                                                                                                                                                                                                                                                                                                                                         |
| --mode [run, compare, normalize] | Specify whether to just run the tests, to also compare results (default), or only check that results are normalized. Just running is useful to generate result files. In 'run' mode tests will not fail unless a command fails.                                                                                                                                                                                                                                                                       |
| --opt-off                        | Disables optimizations (always) and pushing down to the find layer (when possible). Mostly used for generating an initial results file for differential, multiversion testing. This flag requires `--enableTestCommands=true` to be passed to the MongoD.                                                                                                                                                                                                                                             |
| --out [result, oneline]          | Only available in non-compare modes. **Result:** Generate a new '.results' file from the file being run, with each result in a test's result set appearing on a separate line. Will overwrite existing `.results` files. **Oneline:** Generate a new '.results' file from the file being run, with a test's entire result set appearing on one line. Will overwrite existing `.results` files. All of these apply to every file being run, and will add test numbers to tests if not already present. |
| --populateAndExit                | Drops current data and loads documents and indexes per specification in the `*.test` file. No tests are run. `--drop` and `--load` are implicitly applied.                                                                                                                                                                                                                                                                                                                                            |
| --diff [plain, word]             | Specify the type of diff to use when displaying result set differences. Defaults to word-based diff with color if not specified. It is recommended to use the default (`word`) if the terminal `query_tester` is being run in supports ANSI color codes for easier to read output. `plain` uses line based diff with no color.                                                                                                                                                                        |
| --override [queryShapeHash]      | (Optional) Specify what override to use when running a test. When providing the `queryShapeHash` override, it uses the existing corpus of tests but runs explain of the original command instead, extracting the queryShapeHash and asserting that they match the corresponding `file.queryShapeHash.results` file.                                                                                                                                                                                   |

## File types and formats

### .test

See `tests/manual_tests/example/testA.test`. The file format is as follows:
First line must be the testName (matching the filename without the extension).
Second line is the database to run the test against.
Third line is a list of collection files to load. All collections are expected to be in a `collections` directory somewhere along the path to the test file.
Fourth line is a newline noting the end of the header.
After the header, each line is a test line:
`<testType> {commandToRun}`
with each test line being followed by a newline.

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

See `tests/manual_tests/example/testA.results`.
These have the same format as .test files above, with the exception that each test must be followed by a line with the expected documents. These are allowed to be on multiple lines, and a result array is read as the line after the test until the next newline.

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

Some files have a `.queryShapeHash.results` extension. These are the expected results for the queryShapeHash test type, and they only contain the queryShapeHash, as in the `tests/manual_tests/example/testQueryShapeHash.queryShapeHash.results` example.

### .coll

See `tests/manual_tests/example/basic.coll`.
These files are split into two sections divided by an empty line.
Above the empty line are index definitions, one per line. They can be of the form:

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
line with `//`.
Partial-line comments, such as `foo // comment`, are not supported, and the line will be read in its
entirety.

Comments in input `.test` and `.results` files will be persisted in the output as much as possible.

For files containing result set output, comments interleaved between result set lines will be
**ignored** and absent from the output file (see `normalizedCommentsTest.pre` and
`normalizedCommentsTest.results` for an example).

---

[Return to Cover Page](../README_QO.md)
