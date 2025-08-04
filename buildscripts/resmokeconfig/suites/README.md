# Resmoke Test Suites

Test "suites" are configuration files that group which tests to run, and how.

Yaml files enumerate the test files that the suite encompasses, as well as any test fixtures and their configurations to leverage, options for the shell, hooks, and more.

## Minimal Example

`my_suite.yml`:

```yaml
test_kind: js_test
selector:
  roots:
    - jstests/mytests/**/*.js
executor:
  config:
    shell_options:
      nodb: ""
```

This relays the following:

- The suite name is the filename, "my_suite"
- The suite includes all JS files in the `jstests/mytests` directory
- Those tests are run against a shell that is passed the `nodb: ""` options

The following is one with placeholders that illustrate the overall structure:

```yaml
test_kind: js_test
selector:
  roots:
    - jstests/mytests/**/*.js
executor:
  config:
    shell_options:
      nodb: ""
      global_vars:
        TestData:
          defaultReadConcernLevel: null
  hooks:
    - class: ValidateCollections
    - class: CleanEveryN
      n: 20
  fixture:
    class: ShardedClusterFixture
    num_shards: 2
  archive:
    tests: true
    hooks:
      - ValidateCollections
```

# Suite Fields

## `test_kind`

This represents the type of tests that are running in this suite.

Example:

```yaml
test_kind: js_test
```

See all supported kinds in [`buildscripts/resmokelib/testing/testcases`](../../../buildscripts/resmokelib/testing/testcases/README.md).

## `selector`

The selector determines test files to include/exclude in the suite.

Example:

```yaml
selector:
  roots:
    - jstests/aggregation/**/*.js
  exclude_files:
    - jstests/aggregation/extras/*.js
    - jstests/aggregation/data/*.js
  exclude_with_any_tags:
    - requires_pipeline_optimization
```

### `selector.roots`

File path(s) of test files to include. If a path without a glob is provided, it must exist.

### `selector.root`

A file containing glob patterns, one per line, typically used by test_kind cpp_unit_test (usually build/unittests.txt). Specifies which tests to consider for including into the suite. If no other options are specified, these are the tests that will be run. Glob patterns are supported (and common) here.

### `selector.include_files`

A list of strings representing glob patterns. Includes only this subset of tests in the suite. These files will be included even if they would otherwise be excluded by tags. Will error if a test specified here was not included in the roots.

### `selector.exclude_files`

A list of strings representing glob patterns. Excludes this list of tests from the suite. These files will be excluded even if they would otherwise be included by tags. Will error if a test specified here was not included in the roots.

### `selector.include_with_any_tags`

A list of strings. Only jstests which define a list of tags which includes any of these tags will be included in the suite, unless otherwise excluded by filename.

To see all available tags, run `./buildscripts/resmoke.py list-tags`.

### `selector.exclude_with_any_tags`

A list of strings. Any jstest which defines a list of tags which includes any of these tags will be excluded from the suite, unless otherwise included by filename.

To see all available tags, run `./buildscripts/resmoke.py list-tags`.

## `executor`

Defines how the tests will be executed.

### `executor.config`

This section contains additional configuration for each test. The structure of this can vary
significantly based on the `test_kind`. For specific information, you can look at the
implementation of the `test_kind` of concern in the `buildscripts/resmokelib/testing/testcases`
directory.

Example:

```yaml
config:
  shell_options:
    global_vars:
      TestData:
        defaultReadConcernLevel: null
    nodb: ""
    gssapiServiceName: "mockservice"
    eval: >-
      var testingReplication = true;
      load('jstests/libs/override_methods/set_read_and_write_concerns.js');
      load('jstests/libs/override_methods/enable_causal_consistency_without_read_pref.js');
```

Above is an example of the most common `test_kind` -- `js_test`. `js_test` uses `shell_options` to
customize the mongo shell when running tests.

#### `executor.config.shell_options`

Any parameters (besides `global_vars`) will directly be passed to the mongo shell executable.

##### `executor.config.shell_options.global_vars`

Will use this as the base for the string passed to `--eval`. Anything specified in `shell_options.eval` will be appended after these. Formats any objects so that they will evaluate properly as a string.

`global_vars` allows for setting global variables. A `TestData` object is a special global variable
that is used to hold testing data. Parts of `TestData` can be updated via `resmoke` command-line
invocation, via `.yml` (as shown above), and during runtime. The global `TestData` object is merged
intelligently and made available to the `js_test` running. Behavior can vary on key collision, but
in general this is the order of precedence: (1) resmoke command-line (2) [suite].yml (3)
runtime/default.

The mongo shell can also be invoked with flags &
named arguments. Flags must have the `''` value, such as in the case for `nodb` above.

`eval` can also be used to run generic javascript code in the shell. You can directly include
javascript code, or you can put it in a separate script & `load` it.

### `executor.hooks`

Specify hooks to run before, after, and between individual tests to execute specified logic.

> Read more about hooks in [buildscripts/resmokelib/testing/hooks/README.md](../../../buildscripts/resmokelib/testing/hooks/README.md)

The hook name in the `.yml` must match its Python class name of the hook. Parameters can also be included in the `.yml`
and will be passed to the hook's constructor (the `hook_logger` & `fixture` parameters are
automatically included, so those should not be included in the `.yml`).

Example:

```yaml
hooks:
  - class: CheckReplOplogs
  - class: CheckReplDBHash
  - class: ValidateCollections
  - class: CleanEveryN
    n: 20
  - class: MyHook
    param1: something
    param2: somethingelse
```

### `executor.fixture`

Specify a test fixture to run around the tests.

> Read more about fixtures in [buildscripts/resmokelib/testing/fixtures/README.md](../../../buildscripts/resmokelib/testing/fixtures/README.md).

The `class` sub-field corresponds to the Python class name of a fixture. All other sub-fields are passed into the constructor of the fixture. These sub-fields will vary based on the fixture used.

Example:

```yaml
fixture:
  class: ShardedClusterFixture
  num_shards: 2
  mongos_options:
    bind_ip_all: ""
    set_parameters:
      enableTestCommands: 1
  mongod_options:
    bind_ip_all: ""
    set_parameters:
      enableTestCommands: 1
      periodicNoopIntervalSecs: 1
      writePeriodicNoops: true
```

### `executor.archive`

Upon failure, data files can be uploaded to s3. A failure is when a `hook` or `test` throws an
exception. Data files will be archived in the following situations:

1. Any `hook` included in this section throws an exception.
2. If `tests: true` and any `test` in the suite throws an exception.

Example:

```yaml
archive:
  hooks:
    - Hook1
    - Hook2
  tests: true
```

#### `executor.archive.hooks`

Specify a list of hook class names to archive on failure. Set to `true` to archive _all_ hooks.

Read more about [hooks](../../../buildscripts/resmokelib/testing/hooks/README.md).

#### `executor.archive.tests`

Specify a list of test files to archive on failure. Wildcard selection a valid. Set to `true` to archive _all_ tests.
