# Fail Points

[Fail points][fail_point] are test-only configurable hooks that can be triggered at runtime. Fail
points allow tests to change behavior at pre-defined points to block threads, choose rarely executed
branches, enhance diagnostics, or achieve any number of other aims. Fail points can be enabled,
configured, and disabled via command request to a remote process or via an API within the same
process.

For more on what test-only means and how to enable the `configureFailPoint` command, see
[test_commands][test_only].

## Using Fail Points

A fail point must first be defined using `MONGO_FAIL_POINT_DEFINE(myFailPoint)`. This statement adds
the fail point to a registry and allows it to be evaluated in code. There are three common patterns
for evaluating a fail point:

- Exercise a rarely used branch: `if (whenPigsFly || myFailPoint.shouldFail()) { ... }`
- Block until the fail point is unset: `myFailPoint.pauseWhileSet();`
- Use the fail point's payload to perform custom behavior:
  `myFailPoint.execute([](const BSONObj& data) { useMyPayload(data); };`

For more complete usage, see the [fail point header][fail_point] or the [fail point
tests][fail_point_test].

## Configuring and Waiting on Fail Points

Fail point configuration involves choosing a "mode" for activation (e.g., "alwaysOn") and optionally
providing additional data in the form of a BSON object. For the vast majority of cases, this is done
by issuing a `configureFailPoint` command request. This is made easier in JavaScript using the
`configureFailPoint` helper from [fail_point_util.js][fail_point_util]. Fail points can also be
useful in C++ unit tests and integration tests. To configure fail points on the local process, use a
`FailPointEnableBlock` to enable and configure the fail point for a given block scope. Finally, a
fail point can also be set via setParameter by its name prefixed with "failpoint." (e.g.,
"failpoint.myFailPoint").

Users can also wait until a fail point has been evaluated a certain number of times **_over its
lifetime_**. A `waitForFailPoint` command request will send a response back when the fail point has
been evaluated the given number of times. For ease of use, the `configureFailPoint` JavaScript
helper returns an object that can be used to wait a certain amount of times **_from when the fail
point was enabled_**.

For JavaScript examples, see the [fail point JavaScript test][fail_point_javascript_test]. For the
command implementations, see [here][fail_point_commands].

### Waiting on Fail Points in C++

If you hold a `FailPointEnableBlock`, use `waitForOneNewEntry()` / `waitForNNewEntries(n)`. Never
pass an absolute count.

```cpp
FailPointEnableBlock fpb("myFailPoint");
startBackgroundWork();

// Each call counts from when `fpb` enabled the fail point, not from the previous call. Pick one:
fpb.waitForOneNewEntry();          // At least 1 entry since enable.
fpb.waitForNNewEntries(3);         // At least 3 entries since enable.
fpb.waitForOneNewEntry(opCtx);     // Interruptible overloads take an `Interruptible*` first.
fpb.waitForNNewEntries(opCtx, 3);
```

A fail point's `timesEntered` count is cumulative over the lifetime of the process, so an absolute
count passed to `FailPoint::waitForTimesEntered()` can already be satisfied by earlier tests in the
same unittest binary. Never hand-roll the relative count with
`fpb->waitForTimesEntered(fpb.initialTimesEntered() + n)` either; use the methods above.

Each method returns the number of entries since the block enabled the fail point, so you can assert
an exact count: `ASSERT_EQ(1, fpb.waitForOneNewEntry());`

If you enable a fail point with `FailPoint::setMode()` rather than a `FailPointEnableBlock`, use the
count `setMode()` returns as the base: `auto n = fp->setMode(...); fp->waitForTimesEntered(n + 1);`.

A lint rule may enforce this in the future.

## The `failCommand` Fail Point

The `failCommand` fail point is a special fail point used to mock arbitrary response behaviors to
requests filtered by command, appName, etc. It is most often used to simulate specific conditions
between nodes like invalid replica set configurations. For examples of use, see the [failCommand
JavaScript tests][fail_command_javascript_test].

[fail_point]: ../src/mongo/util/fail_point.h
[fail_point_test]: ../src/mongo/util/fail_point_test.cpp
[fail_point_commands]: ../src/mongo/db/commands/fail_point_cmd.cpp
[fail_point_util]: ../jstests/libs/fail_point_util.js
[fail_point_javascript_test]: ../jstests/fail_point/fail_point.js
[fail_command_javascript_test]: ../jstests/core/testing/failcommand_failpoint.js
[test_only]: test_commands.md
