# Server-Internal Baton Pattern

Batons are lightweight job queues in _mongod_ and _mongos_ processes that allow
recording the intent to execute a task (e.g., polling on a network socket) and
deferring its execution to a later time. Batons, often by reusing `Client`
threads and through the _Waitable_ interface, move the execution of scheduled
tasks out of the line, potentially hiding the execution cost from the critical
path. A total of four baton classes are available today:

- [Baton][baton]
- [DefaultBaton][defaultBaton]
- [NetworkingBaton][networkingBaton]
- [AsioNetworkingBaton][asioNetworkingBaton]

## Baton Basics

All baton implementations extend _Baton_. They are tightly associated with an
`OperationContext` and its `Client` thread. An `OperationContext` that belongs
to a `ServiceContext` with a `TransportLayer` uses an `AsioNetworkingBaton`,
else a `DefaultBaton`. The baton is accessed through the `OperationContext` with
a call to `OperationContext::getBaton()`.

Each baton implementation exposes an interface to allow scheduling tasks on the
baton, to demand the awakening of the baton on client socket disconnect, and to
create a _SubBaton_. A _SubBaton_, for any of the baton types, is essentially a
handle to a local object that proxies scheduling requests to its underlying baton
until it is detached (e.g., through destruction of its handle).

Additionally, a _NetworkingBaton_ enables consumers of a transport layer to
execute I/O themselves, rather than delegating it to other threads. They are
special batons that are able to poll network sockets, which is not feasible
through other baton types. This is essential for minimizing context switches and
improving the readability of stack traces.

A baton runs automatically when blocking on its associated `OperationContext`
with a call to `OperationContext::waitForConditionOrInterrupt()`. Many different
apis that take in or use an _Interruptible_ will eventually call into this method
(e.g. `Future::get(...)`, `OperationContext::sleepUntil(...)`, etc.).

### DefaultBaton

DefaultBaton is the most basic baton implementation. This baton provides the
platform to execute tasks while a client thread awaits an event or a timeout,
essentially paving the way towards utilizing idle cycles of client threads for
useful work. Tasks can be scheduled on this baton through its associated
`OperationContext` and using `OperationContext::getBaton()::schedule(...)`.

Note that because _Baton_ extends an _OutOfLineExecutor_, it can be used as the
executor to run work on an `ExecutorFuture`.

### AsioNetworkingBaton

The AsioNetworkingBaton can schedule and run tasks similarly to the _DefaultBaton_,
but it also implements the _NetworkingBaton_ interface to provide a networking
reactor. It can register sessions to monitor and will utilize `poll(2)` and
`eventfd(2)` to wait until I/O can be performed on the socket or until interrupted.

This baton is primarily used for egress networking where it gets scheduled to send
off a command after a connection is made (see the relevant code [here][asioNetworkingBatonScheduling]).
This means that the AsioNetworkingBaton will normally perform socket I/O without
needing to poll. It only registers a session for polling if another read or
write is needed on the socket (e.g. [registering a session during socket read][asioNetworkingBatonPollingSetup]).

In order for an egress session to use the baton, it must be specified as an
argument to `TaskExecutor::scheduleRemoteCommand(...)`.

Note that this baton is only available for Linux.

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
[asioNetworkingBatonScheduling]: https://github.com/mongodb/mongo/blob/46b8c49b4e13cc4c8389b2822f9e30dd73b81d6e/src/mongo/executor/network_interface_tl.cpp#L910
[asioNetworkingBatonPollingSetup]: https://github.com/mongodb/mongo/blob/eab4ec41cc2b28bf0a38eb813f9690e1bfa6c9a6/src/mongo/transport/asio/asio_session_impl.cpp#L666-L696
[example]: https://github.com/mongodb/mongo/blob/262e5a961fa7221bfba5722aeea2db719f2149f5/src/mongo/s/multi_statement_transaction_requests_sender.cpp#L91-L99
