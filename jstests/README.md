# Javascript Test Guide

At MongoDB we write integration tests in JavaScript. These are tests written to exercise some behavior of a running MongoDB server, replica set, or sharded cluster. This guide aims to provide some general guidelines and best practices on how to write good tests.

## Principles

### Minimize the test case as much as possible while still exercising and testing the desired behavior.

-   For example, if you are testing that document deletion works correctly, it may be entirely sufficient to insert just a single document and then delete that document. Inserting multiple documents would be unnecessary. A guiding principle on this is to ask yourself how easy it would be for a new person coming to this test to quickly understand it. If there are multiple documents being inserted into a collection, in a test that only tests document deletion, a newcomer might ask the question: “is it important that the test uses multiple documents, or incidental?”. It is best if you can remove these kinds of questions from a person’s mind, by keeping only the absolute essential parts of a test.
-   We should always strive for unittesting when possible, so if the functionality you want to test can be covered by a unit test, we should write a unit test instead.

### Add a block comment at the top of the JavaScript test file giving a clear and concise overview of what a test is trying to verify.

-   For tests that are more complicated, a brief description of the test steps might be useful as well.

### Keep debuggability in mind.

-   Assertion error messages should contain all information relevant to debugging the test. This means the server’s response from the failed command should almost always be included in the assertion error message. It can also be helpful to include parameters that vary during the test to avoid requiring the investigator to use the logs/backtrace to determine what the test was attempting to do.
-   Think about how easy it would be to debug your test if something failed and a newcomer only had the logs of the test to look at. This can help guide your decision on what log messages to include and to what level of detail. The jsTestLog function is useful for this, as it is good at visually demarcating different phases of a test. As a tip, run your test a few times and just study the log messages, imagining you are an engineer debugging the test with only these logs to look at. Think about how understandable the logs would be to a newcomer. It is easy to add log messages to a test but then forget to see how they would actually appear.
-   Never insert identical documents unless necessary. It is very useful in debugging to be able to figure out where a given piece of data came from.
-   If a test does the same thing multiple times, consider factoring it out into a library. Shorter running tests are easier to debug and code duplication is always bad.

### Do not hardcode collection or database names, especially if they are used multiple times throughout a test.

It is best to use variable names that attempt to describe what a value is used for. For example, naming a variable that stores a collection name collectionToDrop is much better than just naming the variable collName.

### Make every effort to make your test as deterministic as possible.

-   Non-deterministic tests add noise to our build system and, in general, make it harder for yourself and other engineers to determine if the system really is working correctly or not. Flaky integration tests should be considered bugs, and we should not allow them to be committed to the server codebase. One way to make jstests more deterministic is to use failpoints to force the events happening in expected order. However, if we have to use failpoints to make this test deterministic, we should consider write a unit test instead.
-   Note that our fuzzer and concurrency test suites are often an exception to this rule. In those cases we sometimes give up some level of determinism in order to trigger a wider class of rare edge cases. For targeted JavaScript integration tests, however, highly deterministic tests should be the goal.

### Think hard about all the assumptions that the test relies on.

-   For example, if a certain phase of the test ran much slower or much faster, would it cause your test to fail for the wrong reason?
-   If your test includes hard-coded timeouts, make sure they are set appropriately. If a test is waiting for a certain condition to be true, and the test should not proceed until that condition is met, it is often correct to just wait “indefinitely”, instead of adding some arbitrary timeout value, like 30 seconds. In practice this usually means setting some reasonable upper limit, for example, 10 minutes.
-   Also, for replication tests, make sure data exists on the right nodes at the right time. For example, if you a do a write and don’t explicitly wait for it to replicate, it might not reach a secondary node before you try to do the next step of the test.
-   Does your test require data to be stored persistently? Remember that we have test variants that run on in-memory/ephemeral storage engines
-   There are timeouts in the test suites and we aim to make all tests in the same suite finish before timeout. That says we should always make the test run quickly to keep the test short in terms of duration.

### Make tests fail as early as possible.

-   If something goes wrong early in the test, it’s much harder to diagnose when that error becomes visible much later.
-   Wrap every command in assert.commandWorked, or assert.commandFailedWithCode. There is also assert.commandFailed that won't check the return error code, but we should always try to use assert.commandFailedWithCode to make sure the test won't pass on an unexpected error.

### Be aware of all the configurations and variants that your test might run under.

-   Make sure that your test still works correctly if is run in a different configuration or on a different platform than the one you might have tested on.
-   Varying storage engines and suites can often affect a test’s behavior. For example, maybe your test fails unexpectedly if it runs with authentication turned on with an in-memory storage engine. You don’t have to run a new test on every possible platform before committing it, but you should be confident that your test doesn’t break in an unexpected configuration.

### Avoid assertions that verify properties indirectly.

All assertions in a test should attempt to verify the most specific property possible. For example, if you are trying to test that a certain collection exists, it is better to assert that the collection’s exact name exists in the list of collections, as opposed to verifying that the collection count is equal to 1. The desired collection’s existence is sufficient for the collection count to be 1, but not necessary (a different collection could exist in its place). Be wary of adding these kind of indirect assertions in a test.

## Modules in Practice

We have fully migrated to the modularized JavaScript world so any new test should use modules and adapt the new style.

### Only import/export what you need.

It's always important to keep the test context clean so we should only import/export what we need.

-   The unused import is against [no-unused-vars](https://eslint.org/docs/latest/rules/no-unused-vars) rule in ESLint though we haven't enforced it.
-   We don't have a linter to check export since it's hard to tell the necessity, but we should only export the modules that are imported by other tests or will be needed in the future.

### Declare variables in proper scope.

In the past, we have seen tests referring some "undeclared" or "redeclared" variables, which are actually introduced through `load()`. Now with modules, the scope is more clear. We can use global variables properly to setup the test and don't need to worry about polluting other tests.

### Name variables properly when exporting.

To avoid naming conflicts, we should not make the name of exported variables too general which could easily conflict with another variable from the test which import your module. For example, in the following case, the module exported a variable named `alphabet` and it will lead to a re-declaration error.

```
import {alphabet} from "/matts/module.js";
const alphabet = "xyz"; // ERROR
```

### Prefer let/const over var

`let/const` should be preferred over `var` since these can help detect double declaration at the first place. Like, in the naming conflict example, if the second line is using var, it could easily mess up without throwing an error.

### Export in ES6 style

Due to legacy, we have a lot of code that is using the old style to do export, like the following.

```
const MyModule = (function() {
function myFeature() {}
function myOtherFeature() {}

return {myFeature, myOtherFeature};
})();
```

Instead, we should use the ES6 way to do export, as follows.

```
export function myFeature() {}
export function myOtherFeature() {}

// When import from test
import * as MyModule from "/path/to/my_module.js";
```

This can help the language server to discover the methods and provide code navigation for it.
