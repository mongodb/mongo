---
title: FuzzTest
---

FuzzTest is a coverage-guided fuzzing framework for C++ that integrates directly with GoogleTest.
FuzzTest lets you write _property-based tests_: you describe the shape of your inputs using typed
_domains_, and the framework generates and mutates values that satisfy those constraints. FuzzTest
uses Centipede as its fuzzing engine and AUBSAN to surface undefined behavior.

# When to use FuzzTest

- Your function under test accepts structured inputs (integers, strings, custom types, BSON objects,
  etc.) rather than an opaque byte blob.
- You want to express correctness properties beyond "does not crash", such as API invariants,
  differential equivalence, or roundtrip symmetry.
- You want a fuzz test that also runs cleanly as a unit test in normal CI, without needing a special
  fuzzer build variant.

# How to use FuzzTest

## The property function and FUZZ_TEST macro

A FuzzTest consists of a _property function_ and a registration macro. The property function is a
plain C++ function whose parameters define the inputs to fuzz. The framework calls it repeatedly
with generated values, looking for any call that triggers an assertion failure or sanitizer error.

```cpp
#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"

void MyFunctionFuzzer(const std::string& input) {
    MyFunction(input);  // sanitizers catch undefined behavior implicitly
}
FUZZ_TEST(MyTestSuite, MyFunctionFuzzer);
```

When no `.WithDomains()` clause is provided, each parameter defaults to `fuzztest::Arbitrary<T>()`,
which covers most standard library types.

## Specifying input domains

Use `.WithDomains()` to constrain the generated inputs:

> ⚠️ **Warning:** Never initialize input domains with global objects initialized in other
> compilation units. For more information see
> [Fuzz_Test Macro](https://github.com/google/fuzztest/blob/main/doc/fuzz-test-macro.md)

```cpp
void ProcessRequestFuzzer(int opcode, const std::string& payload) {
    ProcessRequest(opcode, payload);
}
FUZZ_TEST(MyTestSuite, ProcessRequestFuzzer)
    .WithDomains(/*opcode=*/fuzztest::InRange(1, 255),
                 /*payload=*/fuzztest::Arbitrary<std::string>());
```

FuzzTest ships with a rich set of built-in domains. A complete list of default types implemented in
fuzztest can be found in the
[Fuzztest Domain Reference](https://github.com/google/fuzztest/blob/main/doc/domains-reference.md).
Also see [BSON Fuzzing](#fuzzing-bson).

## Providing seeds

Seed values give the fuzzer a head start by providing known-interesting inputs to mutate:

> ⚠️ **Warning:** Never initialize seeds with global objects initialized in other compilation units.
> For more information see
> [Fuzz_Test Macro](https://github.com/google/fuzztest/blob/main/doc/fuzz-test-macro.md)

```cpp
FUZZ_TEST(MyTestSuite, ProcessRequestFuzzer)
    .WithDomains(fuzztest::InRange(1, 255),
                 fuzztest::Arbitrary<std::string>())
    .WithSeeds({{1, "hello"}, {255, ""}});
```

You can also load seeds from a directory checked into the repository:

```cpp
FUZZ_TEST(MyTestSuite, ProcessRequestFuzzer)
    .WithSeeds(fuzztest::ReadFilesFromDirectory(
        absl::StrCat(std::getenv("TEST_SRCDIR"), "/path/to/corpus")));
```

## Common correctness patterns

Beyond "does not crash", FuzzTest makes it easy to assert higher-level properties.

**Roundtrip**: verify that encode→decode (or serialize→parse) is the identity:

```cpp
void SerializeRoundtrips(const MyMessage& msg) {
    auto serialized = Serialize(msg);
    auto parsed = Parse(serialized);
    EXPECT_EQ(msg, parsed);
}
FUZZ_TEST(MyTestSuite, SerializeRoundtrips);
```

**Differential fuzzing**: compare two implementations of the same operation:

```cpp
void ImplementationsAgree(const std::string& input) {
    EXPECT_EQ(NewImpl(input), OldImpl(input));
}
FUZZ_TEST(MyTestSuite, ImplementationsAgree);
```

## Using fixtures

If your test requires expensive one-time setup (e.g. starting a service), use a fixture with
`FUZZ_TEST_F`. Any default-constructible class can be a fixture; the constructor and destructor run
once for the whole fuzz test, not once per iteration. When using fixtures, care should be taken to
ensure that only the initial fixture state is retained. Program state created during a test
_**must**_ not affect or be affected by subsequent iterations.

```cpp
class MyServiceFuzzTest {
public:
    MyServiceFuzzTest() { service_.Start(); }
    ~MyServiceFuzzTest() { service_.Stop(); }

    void RequestFuzzer(const std::string& input) {
        service_.Handle(input);
    }

private:
    MyService service_;
};
FUZZ_TEST_F(MyServiceFuzzTest, RequestFuzzer);
```

## Fuzzing BSON

MongoDB provides a custom FuzzTest domain for generating valid BSON objects:
`mongo::bson_mutator::BSONObjImpl`. It is registered as the `Arbitrary<ConstSharedBuffer>`
specialization, so any fuzz test that accepts a `ConstSharedBuffer` will automatically receive
well-formed BSON.

```cpp
#include "mongo/bson/bson_mutator/bson_mutator.h"

void MyCommandFuzzer(ConstSharedBuffer input) {
    BSONObj obj(input);
    MyCommand(obj);
}
FUZZ_TEST(MyCommandFuzzTest, MyCommandFuzzer);
```

To constrain which fields are present and their types, use the `.With<Type>()` builders:

```cpp
FUZZ_TEST(MyCommandFuzzTest, MyCommandFuzzer)
    .WithDomains(fuzztest::Arbitrary<mongo::ConstSharedBuffer>()
                     .WithInt("count")
                     .WithString("name")
                     .WithLong("limit", fuzztest::InRange(0LL, 1000LL)));
```

Fields added via `.With<Type>()` are not guaranteed to appear in every generated object, which
exercises missing-field error handling as well.

Use `.WithVariant()` when a field may legally hold more than one type:

```cpp
fuzztest::Arbitrary<mongo::ConstSharedBuffer>()
    .WithVariant("value", {
        {BSONType::numberInt,  fuzztest::InRange(0, 100)},
        {BSONType::numberLong, fuzztest::InRange(0LL, 100000LL)},
    });
```

Use `.WithAny()` when a key should be present but its type is unconstrained:

```cpp
fuzztest::Arbitrary<mongo::ConstSharedBuffer>().WithAny("filter");
```

## Keeping fuzz tests fast

The more inputs a fuzz test runs per second, the more code it explores. A slow target also risks
[timeouts](#timeouts) in Evergreen.

- Do expensive setup once in a fixture (`FUZZ_TEST_F`), not on every input.
- Bound input sizes, for example with `.WithMaxSize()`, when the code under test does more than
  linear work in the size of its input.
- Don't block on events that never arrive. Mocked network interfaces, executors, and condition
  variables must respond or time out quickly, or every input will time out.

## Bazel target

Use `mongo_cc_fuzztest` (from `//bazel:mongo_src_rules.bzl`) to declare a fuzz test target. It links
in FuzzTest and GoogleTest automatically:

```python
mongo_cc_fuzztest(
    name = "my_command_fuzztest",
    srcs = ["my_command_fuzztest.cpp"],
    deps = [
        "//src/mongo:base",
        "//src/mongo/db/commands:my_command",
    ],
)
```

# Running FuzzTest

## Unit test mode

Every `FUZZ_TEST` is also a regular GoogleTest test. In unit test mode, the property function is
called a small number of times with minimal inputs. This lets fuzz tests run in ordinary CI
alongside unit tests:

```
bazel test --compiler_type=clang --config=fuzztest --fsan --opt=debug --allocator=system +my_command_fuzztest
```

## Fuzzing mode

Fuzzing mode enables sanitizer and coverage instrumentation and runs the test indefinitely (or until
a crash is found). It requires the `fsan` build configuration. Check our Evergreen configuration for
the current bazel arguments, or run:

```
bazel run --compiler_type=clang --config=fuzztest --fsan --opt=debug --allocator=system +my_command_fuzztest -- \
    --fuzz=MyCommandFuzzTest.MyCommandFuzzer
```

To fuzz all tests in a target for a fixed duration, use `--fuzz_for`:

```
bazel run --compiler_type=clang --config=fuzztest --fsan --opt=debug --allocator=system +my_command_fuzztest -- --fuzz_for=60s
```

## Evergreen

Fuzz tests declared with `mongo_cc_fuzztest` run nightly in the `libfuzzertests!` task on the
`*-debug-aubsan-lite_fuzzer-arm64` build variants. The `libfuzzer` resmoke suite runs each binary
as:

```
<binary> --fuzz_for=1h --time_limit_per_input=600s --corpus_database=corpora --llvm_fuzzer_wrapper_corpus_dir=...
```

`--fuzz_for` applies to each `FUZZ_TEST` in turn, so a binary with five fuzz tests runs for five
hours.

A run fails in any of these cases:

- The binary exits with a nonzero status.
- The corpus database contains a crashing input for the binary, under
  `corpora/<binary>/<Suite.Test>/crashing/`. This includes inputs saved by earlier runs that still
  reproduce.
- The fuzzing engine reports a timeout, even if it could not save the input that caused it. See
  [Timeouts](#timeouts).

### The shared corpus

The corpus database persists between runs. Each run downloads the corpus left by the previous run on
the same build variant, fuzzes, and uploads the result in its place. Each task also attaches its
corpus as the "Input Corpora" artifact, along with the compiled tests.

For each fuzz test, the database holds a `coverage/` corpus, which the engine distills after every
session, and a `crashing/` directory, whose inputs are rechecked after every session and kept only
while they still reproduce.

## Timeouts

Centipede runs inputs in batches of up to 1000 in a separate runner process, with two limits:

- **Per input** (`--time_limit_per_input`, 600s in Evergreen). Without this flag, FuzzTest sets no
  per-input limit.
- **Per batch**, derived from the per-input limit and capped at the session length (`--fuzz_for`).
  It can fire even when no single input reaches the per-input limit, if many slow inputs add up.

When an input times out, the engine reruns its batch one input at a time. If the input times out
again on its own, the engine saves it under `crashing/`. If it doesn't, for example because the
slowdown depends on state left by earlier inputs, nothing is saved. A per-batch timeout never
identifies an input. Resmoke fails the run in every case; search the log for `timeout-exceeded` to
find the report.

The 600s limit is far above the slowest input seen in nightly runs, so a timeout means a hang or a
pathologically slow input. Treat it as a bug. Don't raise the limit to make a target pass, and don't
add `ignore_timeout_reports` to `CENTIPEDE_RUNNER_FLAGS`, which hides hangs from both the engine and
resmoke.

### Investigating a timeout

Fuzz the target locally with a much shorter limit, so the slow input times out quickly and is saved:

```
bazel run --compiler_type=clang --config=fuzztest --fsan --opt=debug --allocator=system +my_command_fuzztest -- \
    --fuzz_for=30m --time_limit_per_input=10s --corpus_database=/tmp/corpora
```

To start from the corpus the nightly run used, first extract the failing task's "Input Corpora"
archive into `/tmp/corpora`. To replay the inputs it saves, rerun the target with
`--corpus_database=/tmp/corpora --reproduce_findings_as_separate_tests`. See
[Reproducing crashes](#reproducing-crashes).

## Useful flags

These are FuzzTest flags and take no prefix: `--time_limit_per_input`, not
`--fuzztest_time_limit_per_input`. The binaries silently ignore flags they don't recognize,
including Centipede's own flags such as `--batch_size`.

| Flag                                     | Effect                                         |
| ---------------------------------------- | ---------------------------------------------- |
| `--fuzz=Suite.Test`                      | Fuzz a single test indefinitely                |
| `--fuzz_for=T`                           | Fuzz each test for duration `T`                |
| `--corpus_database=DIR`                  | Load and save corpora and findings under `DIR` |
| `--rss_limit_mb=N`                       | Abort if memory exceeds N MB                   |
| `--time_limit_per_input=T`               | Report an input as a timeout after `T`         |
| `--reproduce_findings_as_separate_tests` | Reruns tests with crashing inputs              |
| `--helpful`                              | Describes fuzztest's flags                     |

# Debugging Crashes in FuzzTest

## Reproducing crashes

```
bazel run --compiler_type=clang --config=fuzztest --fsan --opt=debug --allocator=system +my_command_fuzztest -- --reproduce_findings_as_separate_tests
```

# References

- [FuzzTest overview](https://github.com/google/fuzztest/blob/main/doc/overview.md)
- [The FUZZ_TEST macro](https://github.com/google/fuzztest/blob/main/doc/fuzz-test-macro.md)
- [Domains reference](https://github.com/google/fuzztest/blob/main/doc/domains-reference.md)
- [Use cases](https://github.com/google/fuzztest/blob/main/doc/use-cases.md)
- [Test fixtures](https://github.com/google/fuzztest/blob/main/doc/fixtures.md)
- [Flags reference](https://github.com/google/fuzztest/blob/main/doc/flags-reference.md)
- [FuzzTest on GitHub](https://github.com/google/fuzztest)
- [BSON Mutator](../src/mongo/bson/bson_mutator/bson_mutator.h)
