# Contextual Singletons

The state of all operations performed on Mongo server processes (i.e., `mongod` and `mongos`) is
tracked and managed by a global singleton called the `ServiceContext`. A `ServiceContext` maintains
an arbitrary number of `Client` objects, which each represent a _logical_ connection to the database
over which operations can be performed. Each operation in turn is managed by a single
`OperationContext`. A `Client` object can only perform a single logical operation at a time, and
thus can only maintain a single `OperationContext` at a time. Note that all of these classes are
heavily _decorated_ (i.e., they inherit from [`Decorable`][decorable-url]), which makes them
dynamically extensible.

## [`ServiceContext`][service-context-url]

A `ServiceContext` represents all of the state of a single Mongo server process, which may be either
a `mongod` or a `mongos`. It creates and manages the previously mentioned `Client`s and
`OperationContext`s, as well as a `TransportLayer` for performing network operations, a
`PeriodicRunner` for running housekeeping tasks periodically, a `StorageEngine` for interacting
with the actual database itself, and a set of time sources. In general, every Mongo server process
has a single `ServiceContext`, known as the _global_ `ServiceContext`. Typical uses of the global
`ServiceContext` outside of server initialization and shutdown include looking up `Client` or
`OperationContext` information for a particular thread or operation, or killing one or more running
operations during, e.g., a primary replica step-down. The global `ServiceContext` is created during
initialization of a Mongo server process and is only destroyed at shutdown, and is thus available
for the entire duration of server operation. At shutdown, the global `ServiceContext` will kill all
outstanding `OperationContext`s and `Client`s.

The `ServiceContext` associated with a given `Client` object can be fetched in a few ways; prefer
using [`Client::getServiceContext()`][client-get-service-context-url] when possible. As of time of
writing, every server process only maintains a single `ServiceContext`, but preferring
`Client::getServiceContext()` or `ServiceContext::getCurrentServiceContext()` over
[`ServiceContext::getGlobalServiceContext()`][get-global-service-context-url] will allow us to
more easily maintain multiple `ServiceContext`s per server process if desired in the future.

## [`Client`][client-url]

Each logical connection to a Mongo service is managed by a `Client` object, where a logical
connection may be a user or an internal process that needs to run a command or query on the database.
Construction of a `Client` object is typically performed with a call to `makeClient` on the global
`ServiceContext`, which can then be attached to any thread of execution, or with a call to
[`Client::initThread`][client-init-thread-url] which constructs a `Client` on the global
`ServiceContext` and binds it to the current thread. All operations executed by the `Client` will
take place on that `Client`’s associated thread serially over the network connection managed by the
`Session` object that was passed into the `Client`’s constructor. If no `Session` is passed to the
`Client`’s constructor, then the `Client` is assumed to operate on a local database and will perform
no network operations. These `Client`s are sometimes referred to as “local clients”, and are often
used when a Mongo service needs to query its own database.

A `Client` will typically execute multiple operations over the course of its lifetime, spawning an
`OperationContext` for each. Because these operations are executed serially, each `Client` is
associated with up to one `OperationContext` at any given time.

### The `Client` lock

All `Client`s have an associated lock which protects their internal state including their currently
associated `OperationContext` from concurrent access. Any mutation to a `Client`’s associated
`OperationContext` (or other protected internal state) _must_ take the `Client` lock before being
performed, as an `OperationContext` can otherwise be killed and destroyed at any time. A `Client`
thread may read its own internal state without taking the `Client` lock, but _must_ take the
`Client` lock when reading another `Client` thread's internal state. Only a `Client`'s owning thread
may write to its `Client`'s internal state, and must take the lock when doing so. `Client`s
implement the standard lockable interface (`lock()`, `unlock()`, and `try_lock()`) to support these
operations. The semantics of the `Client` lock are summarized in the table below.

| Internal state | `Client`-owning thread | Other threads |
| -------------- | ---------------------- | ------------- |
| reads          | always allowed         | lock required |
| writes         | lock required          | never allowed |

### `Client` thread manipulation

[`Client::cc()`][client-cc-url] may be used to get the `Client` object associated with the currently
executing thread. Prefer passing `Client` objects as parameters over calls to `Client::cc()` when
possible. A [`ThreadClient`][thread-client-url] is an RAII-style class which may be used to construct
and bind a `Client` to the current running thread and automatically unbind it once the `ThreadClient`
goes out of scope. An [`AlternativeClientRegion`][acr-url] is another RAII-style class which may be
used to temporarily bind a `Client` object to the currently running thread (holding any currently
bound `Client` in reserve), rebinding the current thread’s old `Client` to the current thread upon
falling out of scope. [`ClientStrand`][client-strand-url] functions similarly, but also provides an
`Executor` interface for binding a `Client` to an arbitrary thread.

## [`OperationContext`][operation-context-url]

Each operation that executes on a Mongo server (e.g., a query or a command) is managed by its own
`OperationContext`. An `OperationContext` shepherds an operation’s execution from its inception to
either completion or cancellation. Cancellation may be triggered externally, such as from the
controlling `ServiceContext` on a primary step-down, or from a user-issued [`killOp`][kill-op-url]
command; or internally, e.g., when an operation’s deadline has expired. Every `OperationContext` is
associated with a single `Client`, which manages the logical connection to the database over which
the operation will actually be executed. `OperationContext`s are also optionally associated with a
[`Baton`][baton-url], which represents a thread of execution on which networking operations can be
performed asynchronously.

### Interruptibility

`OperationContext`s implement the [`Interruptible`][interruptible-url] interface, which allows them to
be killed by their associated `Client`s (or, by proxy, their owning `ServiceContext`). See
[this comment block][opctx-interruptible-comment-block-url] for more details on when and how
`OperationContext`s are interrupted.

[service-context-url]: https://github.com/mongodb/mongo/blob/ecc6179c18ed1e3b38d7ee244319210b18e24bad/src/mongo/db/service_context.h#L141
[decorable-url]: https://github.com/mongodb/mongo/blob/ecc6179c18ed1e3b38d7ee244319210b18e24bad/src/mongo/util/decorable.h
[client-get-service-context-url]: https://github.com/mongodb/mongo/blob/ecc6179c18ed1e3b38d7ee244319210b18e24bad/src/mongo/db/client.h#L117
[get-global-service-context-url]: https://github.com/mongodb/mongo/blob/ecc6179c18ed1e3b38d7ee244319210b18e24bad/src/mongo/db/service_context.h#L755
[client-url]: https://github.com/mongodb/mongo/blob/ecc6179c18ed1e3b38d7ee244319210b18e24bad/src/mongo/db/client.h
[client-init-thread-url]: https://github.com/mongodb/mongo/blob/ecc6179c18ed1e3b38d7ee244319210b18e24bad/src/mongo/db/client.h#L75
[client-cc-url]: https://github.com/mongodb/mongo/blob/ecc6179c18ed1e3b38d7ee244319210b18e24bad/src/mongo/db/client.h#L372
[thread-client-url]: https://github.com/mongodb/mongo/blob/ecc6179c18ed1e3b38d7ee244319210b18e24bad/src/mongo/db/client.h#L320
[acr-url]: https://github.com/mongodb/mongo/blob/ecc6179c18ed1e3b38d7ee244319210b18e24bad/src/mongo/db/client.h#L347
[client-strand-url]: https://github.com/mongodb/mongo/blob/ecc6179c18ed1e3b38d7ee244319210b18e24bad/src/mongo/db/client_strand.h
[operation-context-url]: https://github.com/mongodb/mongo/blob/ecc6179c18ed1e3b38d7ee244319210b18e24bad/src/mongo/db/operation_context.h
[kill-op-url]: https://docs.mongodb.com/manual/reference/command/killOp/
[baton-url]: https://github.com/mongodb/mongo/blob/ecc6179c18ed1e3b38d7ee244319210b18e24bad/src/mongo/db/baton.h
[interruptible-url]: https://github.com/mongodb/mongo/blob/ecc6179c18ed1e3b38d7ee244319210b18e24bad/src/mongo/util/interruptible.h
[opctx-interruptible-comment-block-url]: https://github.com/mongodb/mongo/blob/ecc6179c18ed1e3b38d7ee244319210b18e24bad/src/mongo/db/operation_context.cpp#L281
