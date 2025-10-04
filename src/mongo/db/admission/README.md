# Global Lock Admission Control

There are 2 separate ticketing mechanisms placed in front of the global lock acquisition. Both aim to limit the number of concurrent operations from overwhelming the system. Before an operation can acquire the global lock, it must acquire a ticket from one, or both, of the ticketing mechanisms. When both ticket mechanisms are necessary, the acquisition order is as follows:

1. [Flow Control][] - Required only for global lock requests in MODE_IX
2. [Execution Control] - Required for all global lock requests

[Execution Control]: #execution-control
[Flow Control]: ../repl/README.md#flow-control

# Execution Control

Execution control limits the number of concurrent storage engine transactions in a single mongod to
reduce contention on storage engine resources.

## Ticket Management

There are 2 separate pools of available tickets: one pool for global lock read requests
(MODE_S/MODE_IS), and one pool of tickets for global lock write requests (MODE_IX).

As of v7.0, the size of each ticket pool is managed dynamically by the server to maximize
throughput. Details of the algorithm can be found in [Throughput Probing](#throughput-probing) This
dynamic management can be disabled by specifying the size of each pool manually via server
parameters `storageEngineConcurrentReadTransactions` (read ticket pool) and
`storageEngineConcurrentWriteTransactions` (write ticket pool). #

Each pool of tickets is maintained in a
[TicketHolder](https://github.com/mongodb/mongo/blob/r6.3.0-rc0/src/mongo/util/concurrency/ticketholder.h#L52).
Tickets distributed from a given TicketHolder will always be returned to the same TicketHolder (a
write ticket will always be returned to the TicketHolder with the write ticket pool).

## Throughput Probing

Execution control limits concurrency with a throughput-probing algorithm, described below.

### Server Parameters

- `throughputProbingInitialConcurrency -> gInitialConcurrency`: initial number of concurrent read
  and write transactions
- `throughputProbingMinConcurrency -> gMinConcurrency`: minimum concurrent read and write
  transactions
- `throughputProbingMaxConcurrency -> gMaxConcurrency`: maximum concurrent read and write
  transactions
- `throughputProbingReadWriteRatio -> gReadWriteRatio`: ratio of read and write tickets where 0.5
  indicates 1:1 ratio
- `throughputProbingConcurrencyMovingAverageWeight -> gConcurrencyMovingAverageWeight`: weight of
  new concurrency measurement in the exponentially-decaying moving average
- `throughputProbingStepMultiple -> gStepMultiple`: step size for throughput probing

### Pseudocode

```
setConcurrency(concurrency)
    ticketsAllottedToReads := clamp((concurrency * gReadWriteRatio), gMinConcurrency, gMaxConcurrency)
    ticketsAllottedToWrites := clamp((concurrency * (1-gReadWriteRatio)), gMinConcurrency, gMaxConcurrency)

getCurrentConcurrency()
    return ticketsAllocatedToReads + ticketsAllocatedToWrites

exponentialMovingAverage(stableConcurrency, currentConcurrency)
    return (currentConcurrency * gConcurrencyMovingAverageWeight) + (stableConcurrency * (1 - gConcurrencyMovingAverageWeight))

run()
    currentThroughput := (# read tickets returned + # write tickets returned) / time elapsed

    Case of ProbingState
        kStable     probeStable(currentThroughput)
        kUp         probeUp(currentThroughput)
        KDown       probeDown(currentThroughput)

probeStable(currentThroughput)
    stableThroughput := currentThroughput
    currentConcurrency := getCurrentConcurrency()
    if (currentConcurrency < gMaxConcurrency && tickets exhausted)
        setConcurrency(stableConcurrency * (1 + gStepMultiple))
        ProbingState := kUp
    else if (currentConcurrency > gMinConcurrency)
        setConcurrency(stableConcurrency * (1 - gStepMultiple))
        ProbingState := kDown
    else (currentConcurrency == gMinConcurrency), no changes

probeUp(currentThroughput)
    if (currentThroughput > stableThroughput)
        stableConcurrency := exponentialMovingAverage(stableConcurrency, getCurrentConcurrency())
        stableThroughput := currentThroughput
    setConcurrency(stableConcurrency)
    ProbingState := kStable

probeDown(currentThroughput)
    if (currentThroughput > stableThroughput)
        stableConcurrency := exponentialMovingAverage(stableConcurrency, getCurrentConcurrency())
        stableThroughput := currentThroughput
    setConcurrency(stableConcurrency)
    ProbingState := kStable

```

### Diagram

```mermaid
flowchart TB
A(Stable Probe) --> |at minimum and tickets not exhausted|A

A --> |"(above minimum and tickets not exhausted) or at maximum"|C(Probe Down)
subgraph decrease
C --> |throughput increased|F{{Decrease stable concurrency}}
C --> |throughput did not increase|G{{Go back to stable concurrency}}
end
F --> H
G --> H

A --> |below maximum and tickets exhausted| B(Probe Up)
subgraph increase
B --> |throughput increased|D{{Increase stable concurrency}}
B --> |throughput did not increase|E{{Go back to stable concurrency}}
end
D --> H(Stable Probe)
E --> H
```

## Admission Priority

As operations gets admitted through execution control and ingress admission, they get tickets from
those queues. Associated with every ticket is an admission priority for that admission. By default,
tickets have 'normal' priority. For ingress admission, operations created by a process-internal
client are exempted when performing the ingress admission check. For execution control, exemption
must be set explicitly.

In the Flow Control ticketing system, operations of 'immediate' priority bypass ticket acquisition
regardless of ticket availability. Tickets that are not 'immediate' priority must throttle when
there are no tickets available in both Flow Control and Execution Control.

Flow Control is only concerned whether an operation is 'immediate' priority.
The current version of Execution Control only takes into account if the priority is exempt or not,
as exempt priority is the only one beside normal priority.

**AdmissionContext::Priority**

- `kExempt` - Reserved for operations critical to availability (e.g replication workers), or
  observability (e.g. FTDC), and any operation releasing resources (e.g. committing or aborting
  prepared transactions).
- `kNormal` - An operation that should be throttled when the server is under load. If an operation
  is throttled, it will not affect availability or observability. Most operations, both user and
  internal, should use this priority unless they qualify as 'kExempt' priority.

[See AdmissionContext::Priority for more
details](https://github.com/mongodb/mongo/blob/r8.0.9/src/mongo/util/concurrency/admission_context.h#L49-L71).

### How to Exempt an Operation

Some operation may need to be exempt from execution control. In the cases where an operation must
be explicitly set as exempt, use the RAII type
[ScopedAdmissionPriority](https://github.com/mongodb/mongo/blob/r8.0.9/src/mongo/util/concurrency/admission_context.h#L136).

```
ScopedAdmissionPriority<ExecutionAdmissionContext> priority(opCtx, AdmissionContext::Priority::kExempt);
```

# RateLimiter

The `RateLimiter` is not an admission mechanism by itself, but rather a component upon which other admission mechanisms are built. It's implemented as a thin wrapper around [Folly's implementation](https://github.com/mongodb/mongo/blob/e28dc659a386bd80f31bdc01175303c3a043d2c9/src/third_party/folly/dist/folly/TokenBucket.h) of a [Token Bucket](https://en.wikipedia.org/wiki/Token_bucket). In this implementation, the token bucket doesn't actually get "refilled" by a background thread or job. Instead, at each acquisition it atomically calculates whether or not a token is available based on the known refill rate and capacity last time a token was acquired.

`RateLimiter` supports two forms of token acquisition: `tryAcquireToken`, which returns whether a token was acquired or not immediately, and `acquireToken`, which supports queueing up to a configurable maxQueueDepth, with support for interruptibility provided through `OperationContext`. The queuing itself is implemented by "borrowing" tokens from the bucket and sleeping until the time at which that "borrow" would have been a valid token acquisition. (e.g. if the bucket is at 0 capacity with a refill rate of 1 token/s, the thread would borrow 1 token, making the capacity -1, and then sleep for 1s, at which point the bucket has returned to 0 tokens).

# Session Establishment Rate Limiter

The `SessionEstablishmentRateLimiter` places a limit on the number of connections the server (either mongod or mongos) will establish per second. If the rate of attempted establishments exceeds the configured rate, such establishments will queue until either the remote client disconnects or the front of the queue is reached. If the length of the queue reaches the max configured queue depth (if any), the establishment will be rejected and the connection closed.

## Code structure and Components

Each `SessionManager` owns an instance of `SessionEstablishmentRateLimiter`, which is a thin wrapper around a `RateLimiter`.

## Configuration

The `SessionEstablishmentRateLimiter` is controlled by the following server parameters:

- `ingressConnectionEstablishmentRateLimiterEnabled` (bool, default: false): determines whether the rate limiter is enabled or not. Set at startup and runtime.
- `ingressConnectionEstablishmentRatePerSec` (int32_t, default: INT_MAX): the number of new connections that will be allowed to establish per second. Set at startup and runtime.
- `ingressConnectionEstablishmentBurstCapacitySecs` (double, default: DBL_MAX): Describes how many seconds worth of unutilized rate limit can be stored away for use during periods where the rate limit is temporarily exceeded. Set at startup and runtime.
- `ingressConnectionEstablishmentMaxQueueDepth` (int32, default: 0): the maximum size of the connection establishment queue, after which the server will begin rejecting new connections. The default is 0, which means that the server will reject all connections that would queue. Set at startup and runtime.
- `ingressConnectionEstablishmentRateLimiterBypass` (document, default: {}): a document containing a list of CIDR ranges to be exempted from the max establishing limits. Set at startup and runtime.

## Admission Token Acquisition

During the first iteration of a `SessionWorkflow`, the session thread will attempt to acquire an admission token by invoking `SessionEstablishmentRateLimiter::throttleIfNeeded` if `ingressConnectionEstablishmentRateLimiterEnabled` is true. If the remote IP address is not included in one of the ranges specified in `ingressConnectionEstablishmentRateLimiterBypass`, the thread will attempt to acquire a token. If there is capacity in the underlying token bucket, the thread will receive a token and proceed with the initial loop of the `SessionWorkflow`. Otherwise, if the queue has not exceeded `ingressConnectionEstablishmentMaxQueueDepth`, the thread will block until it receives one or the blocking is interrupted by shutdown or the remote client disconnecting.

## Metrics

The following metrics were introduced to `serverStatus`:

```json
"queues.ingressSessionEstablishment": {
    "addedToQueue": 20,
    "removedFromQueue": 10,
    "interruptedInQueue": 5
    "rejectedAdmissions": 10,
    "exemptedAdmissions": 5,
    "successfulAdmissions": 1000,
    "attemptedAdmissions": 1100,
    "averageTimeQueuedMicros": 10,
    "totalAvailableTokens": 0,
}

"metrics.network": {
    "averageTimeToCompletedTLSHandshakeMicros": 23235,
    "averageTimeToCompletedHelloMicros": 35435,
    "averageTimeToCompletedAuthMicros": 45645,
}

"connections": {
    "queuedForEstablishment": 10,
    "establishmentRateLimit": {
        "rejected": 5,
        "exempted": 5,
        "interruptedDueToClientDisconnect": 7
    },
}
```

# Ingress Request Rate Limiting

The `IngressRequestRateLimiter` places a limit on the number of requests that the server (either mongod or mongos) will allow to enter the system per second. Requests that do not receive admission will be rejected with an error response containing the SystemOverloaded error label. This serves two purposes: it prevents the server from being overwhelmed with more requests than it can process, and it also acts as a form of backpressure. In the future, clients such as drivers and mongos will use this backpressure to inform their retry policies and routing decisions to relieve pressure on the system. The point of admission is placed early in the command path (just [after reading the message](https://github.com/mongodb/mongo/blob/da002b73138c7c8b2da57ab10520776de3192978/src/mongo/transport/session_workflow.cpp#L782-L786) from the transport session in `SessionWorkflow`) to make rejection as cheap as possible.

## Differences with the Data-Node Ingress Admission Controller

While the `IngressRequestRateLimiter` has a similar objective to the `IngressAdmissionController`, it attempts to achieve it in a fundamentally different manner. The `IngressAdmissionController` is implemented as a ticket holder, which limits the maximum concurrency of the system by requiring each running thread to acquire a ticket before proceeding. Threads that cannot receive a ticket will block until they can do so. The `IngressRequestRateLimiter` instead limits how many requests _are allowed to enter_ the system _per unit of time_, rather than how many threads can _be proceeding at any given point in time_. This difference allows it to compose well with other existing concurrency limiters deeper in the system, such as the connection pools in mongos or execution control in mongod, by controlling the length of their queues. This is important because rejecting work up front and keeping the queues short allows the server to preserve availability without sacrificing the latency of every request, which would be required if instead all requests were admitted and then queued endlessly when approaching maximum load. Requests that are not admitted can instead either queue client side until they are more likely to succeed in a retry, saving server resources, or be retried immediately on a different server that would be able service it quickly.

## Code Structure and Components

The [`IngressRequestRateLimiter`](https://github.com/mongodb/mongo/blob/e28dc659a386bd80f31bdc01175303c3a043d2c9/src/mongo/db/admission/ingress_request_rate_limiter.h) is stored as a decoration on `ServiceContext`. It is implemented as a thin wrapper around a [`RateLimiter`](https://github.com/mongodb/mongo/blob/e28dc659a386bd80f31bdc01175303c3a043d2c9/src/mongo/db/admission/rate_limiter.h).

## Configuration

The rate limiter is controlled by the following server parameters:

- `ingressRequestRateLimiterEnabled` (bool, default: false): Determines if the rate limiter is enabled at all.
- `ingressRequestAdmissionRatePerSec` (int32, default: INT32_MAX): The number of new requests that will be admitted per second once the burst capacity is consumed. On a technical level, this controls the refill rate of the underlying token bucket.
- `ingressRequestAdmissionBurstCapacitySecs` (double, default: DBL_MAX): Describes how many seconds worth of unutilized rate limit can be stored away to admit additional ingress requests during periods where the rate limit is temporarily exceeded. On a technical level, this describes the capacity of the underlying token bucket in terms of the rate limit.
- `ingressRequestRateLimiterExemptions` (document, default: {}): A document containing a list of CIDR ranges to be exempted from ingress request rate limiting. Acceptable values here follow the same format as the `maxIncomingConnectionsOverride`.

## Admission Token Acquisition

Session threads attempt to acquire an admission token in the `SessionWorkflow` immediately after reading a message from the ingress session if `ingressRequestRateLimiterEnabled` is true and the thread is not exempt from ingress request rate limiting. A thread can be exempt under the following circumstances:

- The remote IP address is included in one of the ranges specified in `ingressRequestRateLimiterExemptions`.
- Auth is enabled and the ingress session has not been authenticated yet.
  - This exemption is to prevent unauthenticated clients from consuming all of the rate limiter tokens, causing unavailability.

Because token acquisition currently only takes place in the `SessionWorkflow`, all internal requests are not subject to rate limiting.

If the thread is not considered exempt, it will attempt to acquire a token from the rate limiter. If it is able to do so, it proceeds as normal. Otherwise, the request is rejected with an error labeled with `SystemOverloaded`. Clients will observe this label and interpret the server as being overloaded, modifying their routing and retry logic accordingly.

## Metrics

The following `serverStatus` metrics are emitted by the `IngressRequestRateLimiter` in the `network.ingressRequestRateLimiter` section:

- `attemptedAdmissions`: the total number of requests that attempted to acquire an admission token, excluding exemptions.
- `successfulAdmissions`: the total number of requests that successfully were admitted into the system, excluding exemptions.
- `rejectedAdmissions`: the total number of requests that were rejected by the rate limiter.
- `exemptedAdmissions`: the total number of requests that bypassed the rate limiter due to one of the conditions described above.
- `totalAvailableTokens`: the current capacity of the underlying token bucket.

# Data-Node Ingress Admission Control

### Quick Overview

Ingress Admission Control is the mechanism placed at the data node ingress layer to help prevent
data-bearing nodes from becoming overloaded with operations. This is done through a ticketing system
that is intended to queue incoming operations based on a configurable overload prevention policy.
Simply put, the queue can admit incoming user operations up to the max number of tickets configured,
and any additional operations wait until a ticket is freed up. Ingress Admission Control is not
applied to all commands however. It's enforced to most user admitted operations, while high priority
and critical internal operations are typically exempt. While the number of tickets is defaulted to
[1,000,000][ingressACidl], it is configurable at startup and runtime.

## Code Structure and Components

Ingress admission control can be broken down into just a few parts:

- **IngressAdmissionsContext**: A decoration on the operation context, which inherits from
  `AdmissionContext`. This base class provides metadata and priority, which is used when determining
  if a command is subject to admission control.

- **ScopedAdmissionPriority**: An RAII-style class that sets the admission priority for an
  operation.

- **IngressAdmissionController**: A decoration on the service context that manages ticket pool size,
  and admission of operations. To be able to utilize these mechanisms, `IngressAdmissionController`
  owns a `TicketHolder`, which is capable of acquiring tickets for operations, and resizing the
  ticket pool.

## Admission Control Ticket Acquisition

The full scope of Admission Control happens inside of
[`ExecCommandDatabase::_initiateCommand()`][initiateCommand] within the ServiceEntryPoint.

To begin, the server parameter [`gIngressAdmissionControlEnabled`][admissionServerParam] is checked
to see if admission control is enabled. If true, we continue with admission control evaluation.

Next we check the main trigger for admission control evaluation,
`isSubjectToIngressAdmissionControl()`. Commands will initially be exempt from admission control as
the default `isSubjectToIngressAdmissionControl` is set to return false. However, each command
invocation can have a different override of `isSubjectToIngressAdmissionControl`, depending on if it
should be subject to admission control. Since each operation has their own implementation, there is
no one collective that determines if an operation needs to be evaluated, so this is left up to each
command's own implementation.

Each operation will attempt to acquire a ticket unless an operation is marked **exempt**, or if the
operation is already holding a ticket. Exempt tickets typically are held by high priority and
critical internal operations. Meanwhile, re-entrancy API's like `DBDirectClient`, where a parent
operation will call into a sub-operation, will cause us to re-enter from the admission layer. It's
important that a sub-operation never acquires a new ticket if the parent operation is already
holding one, otherwise we risk deadlocking the system. In both of these cases, we bypass admission
control and set the priority in the `ScopedAdmissionPriority` object to **Exempt**.

When an operation **is** subject to admission control, we attempt to acquire a ticket. If there are
available tickets, we return the ticket immediately and the operation can continue its execution. If
there are no available tickets, the operation will be blocked, and has to wait for one to become
available.

If we find and return a ticket, it will be used for the lifetime of the command, and will be
released when `ExecCommandDatabase` is finished [executing][ticketRelease] the command.

## How to apply Admission Control to your command

With your new command created, you have a few options for implementing Admission Control. If it is a
high priority command or internal command that is critical for system monitoring and health, you
likely want to exempt it from admission control. The virtual parent function will do this [by
default][subjectVirtualFalse]. It is important to scrutinize the list of exempted operations because
it is critical to the systems health that appropriate operations should queue when possible in the
instance of overload.

If you want to apply admission control, you will need to override
`isSubjectToIngressAdmissionControl` [and return true][subjectAdmissionExTrue]. **Most operations
are expected to fall under this category**.

To apply admission control selectively, override `isSubjectToIngressAdmissionControl` and implement
selective logic to determine [when it should be applied][subjectAdmissionFind].

[initiateCommand]: https://github.com/mongodb/mongo/blob/a86c7f5de2a5de4d2f49e40e8970754ec6a5ba6c/src/mongo/db/service_entry_point_shard_role.cpp#L1588
[admissionServerParam]: https://github.com/mongodb/mongo/blob/291b72ec4a8364208d7633d881cddc98787832b8/src/mongo/db/service_entry_point_shard_role.cpp#L1804
[admissionPriority]: https://github.com/mongodb/mongo/blob/291b72ec4a8364208d7633d881cddc98787832b8/src/mongo/db/service_entry_point_shard_role.cpp#L1809
[tryAcquire]: https://github.com/mongodb/mongo/blob/0ed24f52f011fc16cd968368ace216fe7e747723/src/mongo/util/concurrency/ticketholder.cpp#L130
[subjectAdmissionExTrue]: https://github.com/mongodb/mongo/blob/0ed24f52f011fc16cd968368ace216fe7e747723/src/mongo/db/commands/query_cmd/bulk_write.cpp#L1311
[subjectAdmissionFind]: https://github.com/mongodb/mongo/blob/0ed24f52f011fc16cd968368ace216fe7e747723/src/mongo/db/commands/query_cmd/find_cmd.cpp#L385
[subjectVirtualFalse]: https://github.com/mongodb/mongo/blob/0ed24f52f011fc16cd968368ace216fe7e747723/src/mongo/db/commands.h#L956
[ticketRelease]: https://github.com/mongodb/mongo/blob/0ed24f52f011fc16cd968368ace216fe7e747723/src/mongo/db/service_entry_point_shard_role.cpp#L519
[ingressACidl]: https://github.com/mongodb/mongo/blob/cbb6b8543feeb6e110f646bbeb44d8779d838db1/src/mongo/db/admission/ingress_admission_control.idl#L43
