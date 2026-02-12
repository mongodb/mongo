Note: this doc is being continuously updated while changes are being made to the unit test framework.

# Overview

# Features

The MongoDB unit test framework is a thin layer built atop GoogleTest, so most GoogleTest features
(see [Google Test documentation][google_test_docs]) are available for use aside from anything
listed out in [Banned Features](#banned-features). The unit testing framework also includes
enhanced reporting of test output (see
[Enhanced Reporting of Test Output](#enhanced-reporting-of-test-output)).

The core unittest features can be accessed by including the `mongo/unittest/unittest.h` header and
using the `mongo_cc_unit_test` bazel rule.

## GoogleTest Features

### Parameterized tests

Parameterized tests are a GoogleTest feature that allows the same test logic to be run with
different values or types (see GoogleTest docs on
[Value-Parameterized Tests][value_parameterized_tests] and [Typed Tests][typed_tests]).

```cpp
class TestFixture :
    public testing::TestWithParam<mongo::repl::ReadConcernLevel> {
};

INSTANTIATE_TEST_SUITE_P(TestSuite,
                         TestFixture,
                         testing::Values(ReadConcernLevel::kLocalReadConcern,
                                         ReadConcernLevel::kMajorityReadConcern));

TEST_P(TestFixture, MongoTest) {
    ...
    sendRead(GetParam()); // Uses either ReadConcernLevel::kLocalReadConcern or
                          // ReadConcernLevel::kMajorityReadConcern
}
```

### GoogleMock

GoogleMock can be used by including the `mongo/unittest/unittest.h` header. You should never
directly include `<gmock/gmock.h>`. There are matchers for common mongo types such as `BSONObj`
in `mongo/unittest/matcher.h`.

## Banned Features

- `ASSERT_DEATH` - should not be used. Use `DEATH_TEST` instead (see [Death Tests](#death-tests)).

## Upcoming Features

- IDE Integration
- Color output (Evergreen support, filter out color when terminal not detected)
- Output filtering/formatting
- New APIs to help with unit testing developer experience

## Throwing Assertions

Unlike GoogleTest's fatal test assertions, which implements fatal assertions with `return;`, we
throw an exception when our fatal assertions are triggered. This is to avoid limitations of
GoogleTest fatal assertions, such as no fatal assertions allowed in non-void helper functions.

## Enhanced Reporting of Test Output

The Enhanced Reporter improves test reporting by colorizing and formatting output, maintaining
a progress indicator, printing enhanced failure information, and suppressing log output on
passing tests.

These command line flags may be used to configure the Enhanced Reporter:

- `--showEachTest` - turns off any suppresion of log output.
- `--enhancedReporter=<true/false>` - enable all of the Enhanced Reporter behavior described above.

## Death Tests

The MongoDB unit testing framework uses `DEATH_TEST` (with `DEATH_TEST_F`, `DEATH_TEST_REGEX`,
and `DEATH_TEST_REGEX_F` variants) to test code that is expected to cause the process to
terminate. This should replace all uses of the `ASSERT_DEATH` macro from GoogleTest (see
[unittest/death_test.h][death_test_h] for more details).

Similar to GoogleTest, `DEATH_TEST` test suite names should be suffixed with `DeathTest`. For
instance, for a death test intending to be associated with `SuiteName` should use
`SuiteNameDeathTest` (see GoogleTest's [Death Test naming][death_test_naming]).

```cpp
TEST(SuiteName, TestName) {
    ...
}

DEATH_TEST(SuiteNameDeathTest, TestName) {
    ...
}

using FixtureNameDeathTest = FixtureName;
DEATH_TEST_F(FixtureNameDeathTest, TestName) {
    ...
}
```

[death_test_naming]: https://github.com/google/googletest/blob/main/docs/advanced.md#death-test-naming
[death_test_h]: ../src/mongo/unittest/death_test.h
[google_test_docs]: https://github.com/google/googletest/blob/main/docs/primer.md
[value_parameterized_tests]: https://github.com/google/googletest/blob/main/docs/advanced.md#value-parameterized-tests
[typed_tests]: https://github.com/google/googletest/blob/main/docs/advanced.md#typed-tests
