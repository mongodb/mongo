# Server-Internal Baton Pattern

Batons are lightweight job queues in _mongod_ and _mongos_ processes that allow
recording the intent to execute a task (e.g., polling on a network socket) and
deferring its execution to a later time. Batons, often by reusing `Client`
threads and through the _Waitable_ interface, move the execution of scheduled
tasks out of the line, potentially hiding the execution cost from the critical
path. A total of four baton classes are available today:

-   [Baton][baton]
-   [DefaultBaton][defaultBaton]
-   [NetworkingBaton][networkingBaton]
-   [AsioNetworkingBaton][asioNetworkingBaton]

## Baton Hierarchy

All baton implementations extend _Baton_. They all expose an interface to allow
scheduling tasks on the baton, to demand the awakening of the baton on client
socket disconnect, and to create a _SubBaton_. A _SubBaton_, for any of the
baton types, is essentially a handle to a local object that proxies scheduling
requests to its underlying baton until it is detached (e.g., through destruction
of its handle).

Additionally, a _NetworkingBaton_ enables consumers of a transport layer to
execute I/O themselves, rather than delegating it to other threads. They are
special batons that are able to poll network sockets, which is not feasible
through other baton types. This is essential for minimizing context switches and
improving the readability of stack traces.

### DefaultBaton

DefaultBaton is the most basic baton implementation. A default baton is tightly
associated with an `OperationContext`, and its associated `Client` thread. This
baton provides the platform to execute tasks while a client thread awaits an
event or a timeout (e.g., via `OperationContext::sleepUntil(...)`), essentially
paving the way towards utilizing idle cycles of client threads for useful work.
Tasks can be scheduled on this baton through its associated `OperationContext`
and using `OperationContext::getBaton()::schedule(...)`.

Note that this baton is not available for an `OperationContext` that belongs to
a `ServiceContext` with an `AsioTransportLayer` transport layer. In that case,
the aforementioned interface will return a handle to _AsioNetworkingBaton_.

### AsioNetworkingBaton

This baton is only available for Linux and extends _NetworkingBaton_ to
implement a networking reactor. It utilizes `poll(2)` and `eventfd(2)` to allow
client threads await events without busy polling.

## Example

For an example of scheduling a task on the `OperationContext` baton, see
[here][example].

## Considerations

Since any task scheduled on a baton is intended for out-of-line execution, it
must be non-blocking and preferably short-lived to ensure forward progress.

[baton]: https://github.com/mongodb/mongo/blob/5906d967c3144d09fab6a4cc1daddb295df19ffb/src/mongo/db/baton.h#L61-L178
[defaultBaton]: https://github.com/mongodb/mongo/blob/9cfe13115e92a43d1b9273ee1d5817d548264ba7/src/mongo/db/default_baton.h#L46-L75
[networkingBaton]: https://github.com/mongodb/mongo/blob/9cfe13115e92a43d1b9273ee1d5817d548264ba7/src/mongo/transport/baton.h#L61-L96
[asioNetworkingBaton]: https://github.com/mongodb/mongo/blob/9cfe13115e92a43d1b9273ee1d5817d548264ba7/src/mongo/transport/baton_asio_linux.h#L60-L529
[example]: https://github.com/mongodb/mongo/blob/262e5a961fa7221bfba5722aeea2db719f2149f5/src/mongo/s/multi_statement_transaction_requests_sender.cpp#L91-L99
