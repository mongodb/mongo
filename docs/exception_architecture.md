# Exception Architecture

MongoDB code uses the following types of assertions that are available for use:

-   `uassert` and `iassert`
    -   Checks for per-operation user errors. Operation-fatal.
-   `tassert`
    -   Like uassert in that it checks for per-operation user errors, but inhibits clean shutdown
        in tests. Operation-fatal, but process-fatal in testing environments during shutdown.
-   `massert`
    -   Checks per-operation invariants. Operation-fatal.
-   `fassert`
    -   Checks fatal process invariants. Process-fatal. Use to detect unexpected situations (such
        as a system function returning an unexpected error status).
-   `invariant`
    -   Checks process invariant. Process-fatal. Use to detect code logic errors ("pointer should
        never be null", "we should always be locked").

**Note**: Calling C function `assert` is not allowed. Use one of the above instead.

The following types of assertions are deprecated:

-   `MONGO_verify`
    -   Checks per-operation invariants. A synonym for massert but doesn't require an error code.
        Process fatal in debug mode. Do not use for new code; use invariant or fassert instead.
-   `dassert`
    -   Calls `invariant` but only in debug mode. Do not use!

MongoDB uses a series of `ErrorCodes` (defined in [mongo/base/error_codes.yml][error_codes_yml]) to
identify and categorize error conditions. `ErrorCodes` are defined in a YAML file and converted to
C++ files using [MongoDB's IDL parser][idlc_py] at compile time. We also use error codes to create
`Status` objects, which convey the success or failure of function invocations across the code base.
`Status` objects are also used internally by `DBException`, MongoDB's primary exception class, and
its children (e.g., `AssertionException`) as a means of maintaining metadata for exceptions. The
proper usage of these constructs is described below.

## Assertion Counters

Some assertions will increment an assertion counter. The `serverStatus` command will generate an
"asserts" section including these counters:

-   `regular`
    -   Incremented by `MONGO_verify`.
-   `warning`
    -   Always 0. Nothing increments this anymore.
-   `msg`
    -   Incremented by `massert`.
-   `user`
    -   Incremented by `uassert`.
-   `tripwire`
    -   Incremented by `tassert`.
-   `rollovers`
    -   When any counter reaches a value of `1 << 30`, all of the counters are reset and
        the "rollovers" counter is incremented.

## Considerations

When per-operation invariant checks fail, the current operation fails, but the process and
connection persist. This means that `massert`, `uassert`, `iassert` and `MONGO_verify` only
terminate the current operation, not the whole process. Be careful not to corrupt process state by
mistakenly using these assertions midway through mutating process state.

`fassert` failures will terminate the entire process; this is used for low-level checks where
continuing might lead to corrupt data or loss of data on disk. Additionally, `fassert` will log
the assertion message with fatal severity and add a breakpoint before terminating.

`tassert` will fail the operation like `uassert`, but also triggers a "deferred-fatality tripwire
flag". In testing environments, if the tripwire flag is set during shutdown, the process will
invoke the tripwire fatal assertion. In non-testing environments, there will only be a warning
during shutdown that tripwire assertions have failed.

`tassert` presents more diagnostics than `uassert`. `tassert` will log the assertion as an error,
log scoped debug info (for more info, see ScopedDebugInfoStack defined in
[mongo/util/assert_util.h][assert_util_h]), print the stack trace, and add a breakpoint.
The purpose of `tassert` is to ensure that operation failures will cause a test suite to fail
without resorting to different behavior during testing. `tassert` should only be used to check
for unexpected values produced by defined behavior.

Both `massert` and `uassert` take error codes, so that all assertions have codes associated with
them. Currently, programmers are free to provide the error code by either [using a unique location
number](#choosing-a-unique-location-number) or choosing a named code from `ErrorCodes`. Unique location
numbers have no meaning other than a way to associate a log message with a line of code.

`massert` will log the assertion message as an error, while `uassert` will log the message with
debug level of 1 (for more info about log debug level, see [docs/logging.md][logging_md]).

`iassert` provides similar functionality to `uassert`, but it logs at a debug level of 3 and
does not increment user assertion counters. We should always choose `iassert` over `uassert`
when we expect a failure, a failure might be recoverable, or failure accounting is not interesting.

### Choosing a unique location number

The current convention for choosing a unique location number is to use the 5 digit SERVER ticket number
for the ticket being addressed when the assertion is added, followed by a two digit counter to distinguish
between codes added as part of the same ticket. For example, if you're working on SERVER-12345, the first
error code would be 1234500, the second would be 1234501, etc. This convention can also be used for LOGV2
logging id numbers.

The only real constraint for unique location numbers is that they must be unique across the codebase. This is
verified at compile time with a [python script][errorcodes_py].

## Exception

A failed operation-fatal assertion throws an `AssertionException` or a child of that.
The inheritance hierarchy resembles:

-   `std::exception`
    -   `mongo::DBException`
        -   `mongo::AssertionException`
            -   `mongo::UserException`
            -   `mongo::MsgAssertionException`

See util/assert_util.h.

Generally, code in the server should be able to tolerate (e.g., catch) a `DBException`. Server
functions must be structured with exception safety in mind, such that `DBException` can propagate
upwards harmlessly. The code should also expect, and properly handle, `UserException`. We use
[Resource Acquisition Is Initialization][raii] heavily.

## ErrorCodes and Status

MongoDB uses `ErrorCodes` both internally and externally: a subset of error codes (e.g.,
`BadValue`) are used externally to pass errors over the wire and to clients. These error codes are
the means for MongoDB processes (e.g., _mongod_ and _mongo_) to communicate errors, and are visible
to client applications. Other error codes are used internally to indicate the underlying reason for
a failed operation. For instance, `PeriodicJobIsStopped` is an internal error code that is passed
to callback functions running inside a [`PeriodicRunner`][periodic_runner_h] once the runner is
stopped. The internal error codes are for internal use only and must never be returned to clients
(i.e., in a network response).

Zero or more error categories can be assigned to `ErrorCodes`, which allows a single handler to
serve a group of `ErrorCodes`. `RetriableError`, for instance, is an `ErrorCategory` that includes
all retriable `ErrorCodes` (e.g., `HostUnreachable` and `HostNotFound`). This implies that an
operation that fails with any error code in this category can be safely retried. We can use
`ErrorCodes::isA<${category}>(${error})` to check if `error` belongs to `category`. Alternatively,
we can use `ErrorCodes::is${category}(${error})` to check error categories. Both methods provide
similar functionality.

To represent the status of an executed operation (e.g., a command or a function invocation), we
use `Status` objects, which represent an error state or the absence thereof. A `Status` uses the
standardized `ErrorCodes` to determine the underlying cause of an error. It also allows assigning
a textual description, as well as code-specific extra info, to the error code for further
clarification. The extra info is a subclass of `ErrorExtraInfo` and specific to `ErrorCodes`. Look
for `extra` in [here][error_codes_yml] for reference.

MongoDB provides `StatusWith` to enable functions to return an error code or a value without
requiring them to have multiple outputs. This makes exception-free code cleaner by avoiding
functions with multiple out parameters. We can either pass an error code or an actual value to a
`StatusWith` object, indicating failure or success of the operation. For examples of the proper
usage of `StatusWith`, see [mongo/base/status_with.h][status_with_h] and
[mongo/base/status_with_test.cpp][status_with_test_cpp]. It is highly recommended to use `uassert`
or `iassert` over `StatusWith`, and catch exceptions instead of checking `Status` objects
returned from functions. Using `StatusWith` to indicate exceptions, instead of throwing via
`uassert` and `iassert`, makes it very difficult to identify that an error has occurred, and
could lead to the wrong error being propagated.

## Gotchas

Gotchas to watch out for:

-   Generally, do not throw an `AssertionException` directly. Functions like `uasserted()` do work
    beyond just that. In particular, it makes sure that the `getLastError` structures are set up
    properly.
-   Think about the location of your asserts in constructors, as the destructor would not be
    called. But at a minimum, use `wassert` a lot therein, we want to know if something is wrong.
-   Do **not** throw in destructors or allow exceptions to leak out (if you call a function that
    may throw).

[raii]: https://en.wikipedia.org/wiki/Resource_acquisition_is_initialization
[error_codes_yml]: ../src/mongo/base/error_codes.yml
[periodic_runner_h]: ../src/mongo/util/periodic_runner.h
[status_with_h]: ../src/mongo/base/status_with.h
[idlc_py]: ../buildscripts/idl/idlc.py
[status_with_test_cpp]: ../src/mongo/base/status_with_test.cpp
[errorcodes_py]: ../buildscripts/errorcodes.py
[assert_util_h]: ../src/mongo/util/assert_util.h
[logging_md]: logging.md
