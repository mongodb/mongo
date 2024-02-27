# Resmoke Test Suites

Resmoke stores test suites represented as `.yml` files in the `buildscripts/resmokeconfig/suites`
directory. These `.yml` files allow users to spin up a variety of configurations to run tests
against.

# Suite Fields

## test_kind - [Root Level]

This represents the type of tests that are running in this suite. Some examples include: _js_test,
cpp_unit_test, cpp_integration_tests, benchmark_test, fsm_workload_test, etc._ You can see all
available options in the `_SELECTOR_REGISTRY` at `mongo/buildscripts/resmokelib/selector.py`.

Ex:

```yaml
test_kind: js_test
```

## selector - [Root Level]

The selector determines test files to include/exclude in the suite.

Ex:

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

### selector.roots

File path(s) of test files to include. If a path without a glob is provided, it must exist.

### selector.exclude_files

File path(s) of test files to exclude. If a path without a glob is provided, it must exist.

### selector.exclude_with_any_tags

Exclude test files by tag name(s). To see all available tags, run
`./buildscripts/resmoke.py list-tags`.

## executor - [Root Level]

Configuration for the test execution framework.

Ex:

```yaml
executor:
    archive:
---
config:
---
hooks:
---
fixture:
```

### executor.archive

Upon failure, data files can be uploaded to s3. A failure is when a `hook` or `test` throws an
exception. Data files will be archived in the following situations:

1. Any `hook` included in this section throws an exception.
2. If `tests: true` and any `test` in the suite throws an exception.

Ex:

```yaml
archive:
    hooks:
        - Hook1
        - Hook2
---
tests: true
```

### executor.config

This section contains additional configuration for each test. The structure of this can vary
significantly based on the `test_kind`. For specific information, you can look at the
implementation of the `test_kind` of concern in the `buildscripts/resmokelib/testing/testcases`
directory.

Ex:

```yaml
config:
    shell_options:
        global_vars:
            TestData:
                defaultReadConcernLevel: null
                enableMajorityReadConcern: ""
        nodb: ""
        gssapiServiceName: "mockservice"
        eval: >-
            var testingReplication = true;
            load('jstests/libs/override_methods/set_read_and_write_concerns.js');
            load('jstests/libs/override_methods/enable_causal_consistency_without_read_pref.js');
```

Above is an example of the most common `test_kind` -- `js_test`. `js_test` uses `shell_options` to
customize the mongo shell when running tests.

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

### executor.hooks

All hooks inherit from the `buildscripts.resmokelib.testing.hooks.interface.Hook` parent class and
can override any subset of the following empty base methods: `before_suite`, `after_suite`,
`before_test`, `after_test`. At least 1 base method must be overridden, otherwise the hook will
not do anything at all. During test suite execution, each hook runs its custom logic in the
respective scenarios. Some customizable tasks that hooks can perform include: _validating data,
deleting data, performing cleanup_, etc. You can see all existing hooks in the
`buildscripts/resmokelib/testing/hooks` directory.

Ex:

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

The hook name in the `.yml` must match its Python class name in the
`buildscripts/resmokelib/testing/hooks` directory. Parameters can also be included in the `.yml`
and will be passed to the hook's constructor (the `hook_logger` & `fixture` parameters are
automatically included, so those should not be included in the `.yml`).

### executor.fixture

This represents the test fixture to run tests against. The `class` sub-field corresponds to the
Python class name of a fixture in the `buildscripts/resmokelib/testing/fixtures` directory. All
other sub-fields are passed into the constructor of the fixture. These sub-fields will vary based
on the fixture used.

Ex:

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

## Examples

For inspiration on creating a new test suite, you can check out a variety of examples in the
`buildscripts/resmokeconfig/suites` directory.
