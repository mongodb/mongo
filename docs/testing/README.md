# Testing

Most tests for MongoDB are run through resmoke, our test runner and orchestration tool.
The entry point for resmoke can be found at `buildscripts/resmoke.py`

## Concepts

Learn more about related topics using their own targeted documentation:

- [resmoke](../../buildscripts/resmokelib/README.md), the test runner
- [suites](../../buildscripts/resmokeconfig/suites/README.md), how tests are grouped and configured
- [fixtures](../../buildscripts/resmokelib/testing/fixtures/README.md), specify the server topology that tests run against
- [hooks](../../buildscripts/resmokelib/testing/hooks/README.md), logic to run before, after and/or between individual tests
- [testcases](../../buildscripts/resmokelib/testing/testcases/README.md), Python-based unittest interfaces that resmoke can run as different "kinds" of tests.

## Basic Example

First, ensure that your python `venv` is active and up to date:

```
python3 -m venv python3-venv
source python3-venv/bin/activate
buildscripts/poetry_sync.sh
```

and you've built the source binaries to run against, eg:

```
bazel build install-dist-test
```

Now, **run the test content** from one test file:

```
buildscripts/resmoke.py run --suites=no_passthrough jstests/noPassthrough/shell/js/string.js
```

The suite defined in [buildscripts/resmokeconfig/suites/no_passthrough.yml](../../buildscripts/resmokeconfig/suites/no_passthrough.yml) includes that `string.js` file via glob selections, specifies no fixtures, no hooks, and a minimal config for the executor.
