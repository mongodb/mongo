# Exception Architecture

MongoDB code uses the following types of assertions that are available for use:

-   `uassert`
    -   Checks for per-operation user errors. Operation-fatal.
-   `massert`
    -   Checks per-operation invariants. Operation-fatal.
-   `fassert`
    -   Checks fatal process invariants. Process-fatal. Use to detect unexpected situations (such
        as a system function returning an unexpected error status).
-   `invariant`
    -   Checks process invariant. Process-fatal. Use to detect code logic errors ("pointer should
        never be null", "we should always be locked").

__Note__: Calling C function `assert` is not allowed. Use one of the above instead.

The following types of assertions are deprecated:

-   `verify`
    -   Checks per-operation invariants. A synonym for massert but doesn't require an error code.
        Do not use for new code; use invariant or fassert instead.
-   `dassert`
    -   Calls `verify` but only in debug mode. Do not use!


## Considerations

When per-operation invariant checks fail, the current operation fails, but the process and
connection persist. This means that `massert`, `uassert` and `verify` only terminate the current
operation, not the whole process. Be careful not to corrupt process state by mistakenly using these
assertions midway through mutating process state. Examples of this include `uassert` and `massert`
inside of constructors and destructors.

`fassert` failures will terminate the entire process; this is used for low-level checks where
continuing might lead to corrupt data or loss of data on disk.

Both `massert` and uassert take error codes, so that all errors have codes associated with them.
These error codes are assigned incrementally; the numbers have no meaning other than a way to
associate a log message with a line of code. SCons checks for duplicates, but if you want the next
available code you can run:

```
python buildscripts/errorcodes.py
```

## Exception

A failed operation-fatal assertion throws an `AssertionException` or a child of that.
The inheritance hierarchy resembles:

-   `std::exception`
    -   `mongo::DBException`
        -   `mongo::AssertionException`
            -   `mongo::UserException`
            -   `mongo::MsgAssertionException`

See util/assert_util.h.

Generally, code in the server should be prepared to catch a `DBException`. The code should also
expect `UserException`. We use [Resource Acquisition Is Initialization][1] heavily.


## Gotchas

Gotchas to watch out for:

-   Generally, do not throw an `AssertionException` directly. Functions like `uasserted()` do work
    beyond just that. In particular, it makes sure that the `getLastError` structures are set up
    properly.
-   Think about the location of your asserts in constructors, as the destructor would not be
    called. But at a minimum, use `wassert` a lot therein, we want to know if something is wrong.
-   Do __not__ throw in destructors or allow exceptions to leak out (if you call a function that
    may throw).


[1]: https://en.wikipedia.org/wiki/Resource_acquisition_is_initialization
