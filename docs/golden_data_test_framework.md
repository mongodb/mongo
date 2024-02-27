# Overview

Golden Data test framework provides ability to run and manage tests that produce an output which is
verified by comparing it to the checked-in, known valid output. Any differences result in test
failure and either the code or expected output has to be updated.

Golden Data tests excel at bulk diffing of failed test outputs and bulk accepting of new test
outputs.

# When to use Golden Data tests?

-   Code under test produces a deterministic output: That way tests can consistently succeed or fail.
-   Incremental changes to code under test or test fixture result in incremental changes to the
    output.
-   As an alternative to ASSERT for large output comparison: Serves the same purpose, but provides
    tools for diffing/updating.
-   The outputs can't be objectively verified (e.g. by verifying well known properties). Examples:
    -   Verifying if sorting works, can be done by verifying that output is sorted. SHOULD NOT use
        Golden Data tests.
    -   Verifying that pretty printing works, MAY use Golden Data tests to verify the output, as there
        might not be well known properties or those properties can easily change.
-   As stability/versioning/regression testing. Golden Data tests by storing recorded outputs, are
    good candidate for preserving behavior of legacy versions or detecting undesired changes in
    behavior, even in cases when new behavior meets other correctness criteria.

# Best practices for working with Golden Data tests

-   Tests MUST produce text output that is diffable can be inspected in the pull request.

-   Tests MUST produce an output that is deterministic and repeatable. Including running on different
    platforms. Same as with ASSERT_EQ.
-   Tests SHOULD produce an output that changes incrementally in response to the incremental test or
    code changes.

-   Multiple test variations MAY be bundled into a single test. Recommended when testing same feature
    with different inputs. This helps reviewing the outputs by grouping similar tests together, and also
    reduces the number of output files.

-   Changes to test fixture or test code that affect non-trivial amount test outputs MUST BE done in
    separate pull request from production code changes:

    -   Pull request for test code only changes can be easily reviewed, even if large number of test
        outputs are modified. While such changes can still introduce merge conflicts, they don't introduce
        risk of regression (if outputs were valid
    -   Pull requests with mixed production

-   Tests in the same suite SHOULD share the fixtures when appropriate. This reduces cost of adding
    new tests to the suite. Changes to the fixture may only affect expected outputs from that fixtures,
    and those output can be updated in bulk.

-   Tests in different suites SHOULD NOT reuse/share fixtures. Changes to the fixture can affect large
    number of expected outputs.
    There are exceptions to that rule, and tests in different suites MAY reuse/share fixtures if:

    -   Test fixture is considered stable and changes rarely.
    -   Tests suites are related, either by sharing tests, or testing similar components.
    -   Setup/teardown costs are excessive, and sharing the same instance of a fixture for performance
        reasons can't be avoided.

-   Tests SHOULD print both inputs and outputs of the tested code. This makes it easy for reviewers to
    verify of the expected outputs are indeed correct by having both input and output next to each
    other.
    Otherwise finding the input used to produce the new output may not be practical, and might not even
    be included in the diff.

-   When resolving merge conflicts on the expected output files, one of the approaches below SHOULD be
    used:

    -   "Accept theirs", rerun the tests and verify new outputs. This doesn't require knowledge of
        production/test code changes in "theirs" branch, but requires re-review and re-acceptance of c
        hanges done by local branch.
    -   "Accept yours", rerun the tests and verify the new outputs. This approach requires knowledge of
        production/test code changes in "theirs" branch. However, if such changes resulted in
        straightforward and repetitive output changes, like due to printing code change or fixture change,
        it may be easier to verify than reinspecting local changes.

-   Expected test outputs SHOULD be reused across tightly-coupled test suites. The suites are
    tightly-coupled if:

    -   Share the same tests, inputs and fixtures.
    -   Test similar scenarios.
    -   Test different code paths, but changes to one of the code path is expected to be accompanied by
        changes to the other code paths as well.

    Tests SHOULD use different test files, for legitimate and expected output differences between
    those suites.

    Examples:

    -   Functional tests, integration tests and unit tests that test the same behavior in different
        environments.
    -   Versioned tests, where expected behavior is the same for majority of test inputs/scenarios.

-   AVOID manually modifying expected output files. Those files are considered to be auto generated.
    Instead, run the tests and then copy the generated output as a new expected output file. See "How to
    diff and accept new test outputs" section for instructions.

# How to use write Golden Data tests?

Each golden data test should produce a text output that will be later verified. The output format
must be text, but otherwise test author can choose a most appropriate output format (text, json,
bson, yaml or mixed). If a test consists of multiple variations each variation should be clearly
separated from each other.

Note: Test output is usually only written. It is ok to focus on just writing serialization/printing
code without a need to provide deserialization/parsing code.

When actual test output is different from expected output, test framework will fail the test, log
both outputs and also create following files, that can be inspected later:

-   <output_path>/actual/<test_path> - with actual test output
-   <output_path>/expected/<test_path> - with expected test output

## CPP tests

`::mongo::unittest::GoldenTestConfig` - Provides a way to configure test suite(s). Defines where the
expected output files are located in the source repo.

`::mongo::unittest::GoldenTestContext` - Provides an output stream where tests should write their
outputs. Verifies the output with the expected output that is in the source repo

See: [golden_test.h](../src/mongo/unittest/golden_test.h)

**Example:**

```c++
#include "mongo/unittest/golden_test.h"

GoldenTestConfig myConfig("src/mongo/my_expected_output");
TEST(MySuite, MyTest) {
    GoldenTestContext ctx(myConfig);
    ctx.outStream() << "print something here" << std::endl;
    ctx.outStream() << "print something else" << std::endl;
}

void runVariation(GoldenTestContext& ctx, const std::string& variationName, T input) {
    ctx.outStream() << "VARIATION " << variationName << std::endl;
    ctx.outStream() << "input: " << input << std::endl;
    ctx.outStream() << "output: " << runCodeUnderTest(input) << std::endl;
    ctx.outStream() << std::endl;
}

TEST_F(MySuiteFixture, MyFeatureATest) {
    GoldenTestContext ctx(myConfig);
    runMyVariation(ctx, "variation 1", "some input testing A #1")
    runMyVariation(ctx, "variation 2", "some input testing A #2")
    runMyVariation(ctx, "variation 3", "some input testing A #3")
}

TEST_F(MySuiteFixture, MyFeatureBTest) {
    GoldenTestContext ctx(myConfig);
    runMyVariation(ctx, "variation 1", "some input testing B #1")
    runMyVariation(ctx, "variation 2", "some input testing B #2")
    runMyVariation(ctx, "variation 3", "some input testing B #3")
    runMyVariation(ctx, "variation 4", "some input testing B #4")
}
```

Also see self-test:
[golden_test_test.cpp](../src/mongo/unittest/golden_test_test.cpp)

# How to diff and accept new test outputs on a workstation

Use buildscripts/golden_test.py command line tool to manage the test outputs. This includes:

-   diffing all output differences of all tests in a given test run output.
-   accepting all output differences of all tests in a given test run output.

## Setup

buildscripts/golden_test.py requires a one-time workstation setup.

Note: this setup is only required to use buildscripts/golden_test.py itself. It is NOT required to
just run the Golden Data tests when not using buildscripts/golden_test.py.

1. Create a yaml config file, as described by [Appendix - Config file reference](#appendix---config-file-reference).
2. Set GOLDEN_TEST_CONFIG_PATH environment variable to config file location, so that is available
   when running tests and when running buildscripts/golden_test.py tool.

### Automatic Setup

Use buildscripts/golden_test.py builtin setup to initialize default config for your current platform.

**Instructions for Linux**

Run buildscripts/golden_test.py setup utility

```bash
buildscripts/golden_test.py setup
```

**Instructions for Windows**

Run buildscripts/golden_test.py setup utility.
You may be asked for a password, when not running in "Run as administrator" shell.

```cmd
c:\python\python310\python.exe buildscripts/golden_test.py setup
```

### Manual Setup (Default config)

This is the same config as that would be setup by the [Automatic Setup](#automatic-setup)

This config uses a unique subfolder folder for each test run. (default)

-   Allows diffing each test run separately.
-   Works with multiple source repos.

**Instructions for Linux/macOS:**

This config uses a unique subfolder folder for each test run. (default)

-   Allows diffing each test run separately.
-   Works with multiple source repos.

Create ~/.golden_test_config.yml with following contents:

```yaml
outputRootPattern: /var/tmp/test_output/out-%%%%-%%%%-%%%%-%%%%
diffCmd: git diff --no-index "{{expected}}" "{{actual}}"
```

Update .bashrc, .zshrc

```bash
export GOLDEN_TEST_CONFIG_PATH=~/.golden_test_config.yml
```

alternatively modify /etc/environment or other configuration if needed by Debugger/IDE etc.

**Instructions for Windows:**

Create %LocalAppData%\.golden_test_config.yml with the following contents:

```yaml
outputRootPattern: 'C:\Users\Administrator\AppData\Local\Temp\test_output\out-%%%%-%%%%-%%%%-%%%%'
diffCmd: 'git diff --no-index "{{expected}}" "{{actual}}"'
```

Add GOLDEN_TEST_CONFIG_PATH=~/.golden_test_config.yml environment variable:

```cmd
runas /profile /user:administrator "setx GOLDEN_TEST_CONFIG_PATH %LocalAppData%\.golden_test_config.yml"
```

## Usage

### List all available test outputs

```bash
$> buildscripts/golden_test.py list
```

### Diff test results from most recent test run:

```bash
$> buildscripts/golden_test.py diff
```

This will run the diffCmd that was specified in the config file

### Diff test results from most recent test run:

```bash
$> buildscripts/golden_test.py accept
```

This will copy all actual test outputs from that test run to the source repo and new expected
outputs.

### Get paths from most recent test run (to be used by custom tools)

Get expected and actual output paths for most recent test run:

```bash
$> buildscripts/golden_test.py get
```

Get expected and actual output paths for most most recent test run:

```bash
$> buildscripts/golden_test.py get_root
```

Get all available commands and options:

```bash
$> buildscripts/golden_test.py --help
```

# How to diff test results from a non-workstation test run

## Bulk folder diff the results:

1. Parse the test log to find the root output locations where expected and actual output files were
   written.
2. Then compare the folders to see the differences for tests that failed.

**Example: (linux/macOS)**

```bash
# Show the test run expected and actual folders:
$> cat test.log | grep "^{" | jq -s -c -r '.[] | select(.id == 6273501 ) | .attr.expectedOutputRoot + " " +.attr.actualOutputRoot ' | sort | uniq
# Run the recursive diff
$> diff -ruN --unidirectional-new-file --color=always <expected_root> <actual_root>
```

## Find the outputs of tests that failed.

Parse logs and find the the expected and actual outputs for each failed test.

**Example: (linux/macOS)**

```bash
# Find all expected and actual outputs of tests that have failed
$> cat test.log | grep "^{" | jq -s '.[] | select(.id == 6273501 ) | .attr.testPath,.attr.expectedOutput,.attr.actualOutput'
```

# Appendix - Config file reference

Golden Data test config file is a YAML file specified as:

```yaml
outputRootPattern:
    type: String
    optional: true
    description:
        Root path patten that will be used to write expected and actual test outputs for all tests
        in the test run.
        If not specified a temporary folder location will be used.
        Path pattern string may use '%' characters in the last part of the path. '%' characters in
        the last part of the path will be replaced with random lowercase hexadecimal digits.
    examples: /var/tmp/test_output/out-%%%%-%%%%-%%%%-%%%%
        /var/tmp/test_output

diffCmd:
    type: String
    optional: true
    description: Shell command to diff a single golden test run output.
        {{expected}} and {{actual}} variables should be used and will be replaced  with expected and
        actual output folder paths respectively.
        This property is not used to decide whether the test passes or fails; it is only used to
        display differences once we've decided that a test failed.
    examples: git diff --no-index "{{expected}}" "{{actual}}"
        diff -ruN --unidirectional-new-file --color=always "{{expected}}" "{{actual}}"
```
