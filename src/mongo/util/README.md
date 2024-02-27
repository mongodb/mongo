# MongoDB Utilities and Primatives

This document provides a high-level overview of certain utilities and primatives used within the
server. This is a work in progress and more sections will be added gradually.

## Fail Points

For details on the server-internal _FailPoint_ pattern, see [this document][fail_points].

[fail_points]: ../../../docs/fail_points.md

## Cancellation Sources and Tokens

### Intro

When writing asynchronous code, we often schedule code or operations to run at some point in the future, in a different execution context. Sometimes, we want to cancel that scheduled work - to stop it from ever running if it hasn't yet run, and possibly to interrupt its execution if it is safe to do so. For example, in the MongoDB server, we might want to:

-   Cancel work scheduled on executors
-   Cancel asynchronous work chained as continuations on futures
-   Write and use services that asynchronously perform cancelable work for consumers in the background

In the MongoDB server, we have two types that together make it easy to manage the cancellation of this sort of asynchronous work: CancellationSources and CancellationTokens.

### The CancellationSource and CancellationToken Types

A `CancellationSource` manages the cancellation state for some unit of asynchronous work. This unit of asynchronous work can consist of any number of cancelable operations that should all be canceled together, e.g. functions scheduled to run on an executor, continuations on futures, or operations run by a service implementing cancellation. A `CancellationSource` is constructed in an uncanceled state, and cancellation can be requested by calling the member `CancellationSource::cancel()`.

A `CancellationSource` can be used to produce associated CancellationTokens with the member function `CancellationSource::token()`. These CancellationTokens can be passed to asynchronous operations to make them cancelable. By accepting a `CancellationToken` as a parameter, an asynchronous operation signals that it will attempt to cancel the operation when the `CancellationSource` associated with that `CancellationToken` has been canceled.

When passed a `CancellationToken`, asynchronous operations are able to handle the cancellation of the `CancellationSource` associated with that `CancellationToken` in two ways:

-   The `CancellationToken::isCanceled()` member function can be used to check at any point in time if the `CancellationSource` the `CancellationToken` was obtained from has been canceled. The code implementing the asynchronous operation can therefore check the value of this member function at appropriate points and, if the `CancellationSource` has been canceled, refuse to run the work or stop running work if it is ongoing.

-   The `CancellationToken:onCancel()` member function returns a `SemiFuture` that will be resolved successfully when the underlying `CancellationSource` has been canceled or resolved with an error if it is destructed before being canceled. Continuations can therefore be chained on this future that will run when the associated `CancellationSource` has been canceled. Importantly, because `CancellationToken:onCancel()` returns a `SemiFuture`, implementors of asynchronous operations must provide an execution context in which they want their chained continuation to run. Normally, this continuation should be scheduled to run on an executor, by passing one to `SemiFuture::thenRunOn()`.

    -   Alternatively, the continuation can be forced to run inline by transforming the `SemiFuture` into an inline future, by using `SemiFuture::unsafeToInlineFuture()`. This should be used very cautiously. When a continuation is chained to the `CancellationToken:onCancel()` future via `SemiFuture::unsafeToInlineFuture()`, the thread that calls `CancellationSource::cancel()` will be forced to run the continuation inline when it makes that call. Note that this means if a service chains many continuations in this way on `CancellationToken`s obtained from the same `CancellationSource`, then whatever thread calls`CancellationSource::cancel()` on that source will be forced to run all of those continuations potentially blocking that thread from making further progress for a non-trivial amount of time. Do not use `SemiFuture::unsafeToInlineFuture()` in this way unless you are sure you can block the thread that cancels the underlying `CancellationSource` until cancellation is complete. Additionally, remember that because the `SemiFuture` returned by `CancellationToken::onCancel()` is resolved as soon as that `CancellationToken` is canceled, if you attempt to chain a continuation on that future when the `CancellationToken` has _already_ been canceled, that continuation will be ready to run right away. Ordinarily, this just means the continuation will immediately be scheduled on the provided executor, but if `SemiFuture::unsafeToInlineFuture` is used to force the continuation to run inline, it will run inline immediately, potentially leading to deadlocks if you're not careful.

### Example of a Service Performing Cancelable, Asynchronous Work

We'll use the WaitForMajorityService as an example of how work can be scheduled and cancelled using CancellationSources and CancellationTokens. First, we'll see how a consumer might use a service implementing cancellation. Then, we'll see how a service might implement cancellation internally.

#### Using a Cancelable Service

The WaitForMajorityService allows consumers to asynchronously wait until an `opTime` is majority committed. Consumers can request a wait on a specific `opTime` by calling the function `WaitForMajorityService::waitUntilMajority(OpTime opTime, CancellationToken token)`. This call will return a future that will be resolved when that `opTime` has been majority committed or otherwise set with an error.

In some cases, though, a consumer might realize that it no longer wants to wait on an `opTime`. For example, the consumer might be going through shut-down, and it needs to clean up all of its resources cleanly right away. Or, it might just realize that the `opTime` is no longer relevant, and it would like to tell the `WaitForMajorityService` that it no longer needs to wait on it, so the `WaitForMajorityService` can conserve its own resources.

Consumers can easily cancel existing requests to wait on `opTime`s in situations like these by making use of the `CancellationToken` argument accepted by `WaitForMajorityService::waitUntilMajority`. For any `opTime` waits that should be cancelled together, they simply pass `CancellationTokens` from the same `CancellationSource` into the requests to wait on those `opTimes`:

```c++
CancellationSource source;
auto opTimeFuture = WaitForMajorityService::waitUntilMajority(opTime, source.token());
auto opTimeFuture2 = WaitForMajorityService::waitUntilMajority(opTime2, source.token());
```

And whenever they want to cancel the waits on those `opTime`s, they can simply call:

```c++
source.cancel()
```

After this call, the `WaitForMajorityService` will stop waiting on `opTime` and `opTime2`. And the futures returned by all calls to `waitUntilMajority` that were passed `CancellationToken`s from the `CancellationSource source` (in this case, `opTimeFuture` and `opTimeFuture2`) will be resolved with `ErrorCodes::CallbackCanceled`.

#### Implementing a Cancelable Service

Now we'll see how `WaitForMajorityService` might be implemented, at a high level, to support the cancelable API we saw in the last section. The `WaitForMajorityService` will need to ensure that calls to `WaitForMajorityService::waitUntilMajority(opTime, token)` schedule work to wait until `opTime` is majority committed, and that this scheduled work will stop if `token` has been canceled. To do so, it can use either of the functions `CancellationToken` has that expose the underlying cancellation state: it can either call `CancellationToken::isCanceled()` at some appropriate time on `token` and conditionally stop waiting on `opTime`, or it can chain a continuation onto the future returned by `CancellationToken:onCancel()` that will stop the wait. This continuation will run when the `token` is canceled, as cancellation resolves the future returned by `CancellationToken::onCancel()`.

To keep the example general, we're going to elide some details: for now, assume that calling `stopWaiting(opTime)` performs all the work needed for the `WaitForMajorityService` to stop waiting on `opTime`. Additionally, assume that `opTimePromise` is the promise that resolves the future returned by the call to `waitUntilMajority(opTime, token)`. Then, to implement cancellation for some request to wait on `opTime` with an associated token `token`, the `WaitForMajorityService` can add something like the following code to the function it uses to accept requests:

```c++
SemiFuture<void> WaitForMajorityService::waitUntilMajority(OpTime opTime, CancellationToken token) {
    // ... Create request that will be processed by a background thread

    token.onCancel().thenRunOn(executor).getAsync([](Status s) {
        if (s.isOK()) { // The token was canceled.
            stopWaiting(opTime);
            opTimePromise.setError({ErrorCodes::CallbackCanceled, "waitUntilMajority canceled"});
        }
    });
}
```

Whenever `token` is canceled, the continuation above will run, which will stop the `WaitForMajorityService` from waiting on `opTime`, and resolve the future originally returned from the call to `waitUntilMajority(opTime, token)` with an error. There's just one more detail -- we don't want the cancellation and ordinary completion of the work to race. If `token` is canceled _after_ we've finished waiting for opTime to be majority committed, there's no work to cancel, and we can't set opTimePromise twice! To fix this, we can simply protect opTimePromise with an atomic, ensuring that it will be set exactly once. Then, before we perform either cancellation work or fulfilling the promise by ordinary means, we can use the atomic to check that the promise has not already been completed. This is the gist of what it takes to write a service performing cancelable work! To see the full details of making a cancelable `WaitForMajorityService`, see this [commit](https://github.com/mongodb/mongo/commit/4fa2fcb16107c860448b58cd66798bae140e7263).

### Cancellation Hierarchies

In the above example, we saw how we can use a single `CancellationSource` and associated tokens to cancel work. This works well when we can associate a specific group of asynchronous tasks with a single `CancellationSource`. Sometimes, we may want sub-tasks of a larger operation to be individually cancelable, but also to be able to cancel _all_ tasks associated with the larger operation at once. CancellationSources can be managed hierarchically to make this sort of situation manageable. A hierarchy between CancellationSources is created by passing a `CancellationToken` associated with one `CancellationSource` to the constructor of a newly-created `CancellationSource` as follows:

```c++
CancellationSource parentSource;
CancellationSource childSource(parentSource.token());
```

As the naming suggests, we say that the `parentSource` and `childSource` `CancellationSources` are in a hierarchy, with `parentSource` higher up in the hierarchy. When a `CancellationSource` higher up in a cancellation hierarchy is canceled, all descendant `CancellationSources` are automatically canceled as well. Conversely, the `CancellationSources` lower down in a cancellation hierarchy can be canceled without affecting any other `CancellationSources` higher up or at the same level of the hierarchy.

As an example of this sort of hierarchy of cancelable operations, let's consider the case of [hedged reads](https://docs.mongodb.com/manual/core/read-preference-hedge-option/). Note that hedged reads do not currently use `CancellationTokens` in their implementation; this is for example purposes only. When a query is specified with the "hedged read" option on a sharded cluster, mongos will route the read to two replica set members for each shard targeted by the query return the first response it receives per-shard. Therefore, as soon as it receives a response from one replica set member on a shard, it can cancel the other request it made to a different member on the same shard. We can use the pattern discussed above to do this sort of cancellation. At a high level, the code might look something like this:

```c++
// Assuming we already have a CancellationSource called hedgedReadSource for the entire
// hedged-read operation, we create child CancellationSources used to manage the cancellation
// state for each request to a replica set member of the shard
CancellationSource hostOneSource(hedgedReadSource.token());
CancellationSource hostTwoSource(hedgedReadSource.token());

// Send the read query to two different replica set members of some shard
auto readOneFuture = routeRequestToHost(query, host1, hostOneSource.token());
auto readTwoFuture = routeRequestToHost(query, host2, hostTwoSource.token());

// whenAny(F1, F2, ..., FN) takes a list of N future types and returns a future
// that is resolved as soon as any of the input futures are ready. The value of
// the returned future is a struct containing the result of the future that resolved
// as well as its index in the input list.
auto firstResponse = whenAny(std::move(readOneFuture), std::move(readTwoFuture)).then(
    [hostOneSource = std::move(hostOneSource), hostTwoSource = std::move(hostTwoSource)]
    (auto readResultAndIndex)
    {
        if (readResultAndIndex.result.isOK()) {
            if (readResultAndIndex.index == 0) {
                // The result we have was returned from host1; cancel the other request
                hostTwoSource.cancel();
            } else {
                // And vice-versa
                hostOneSource.cancel();
            }
            return readResultAndIndex.result.getValue();
        }
});
```

We can see the utility of the hierarchy of `CancellationSources` by examining the case where the client indicates that it would like to kill the entire hedged read operation. Rather than having to track every `CancellationSource` used to manage different requests performed throughout the operation, we can call

```c++
hedgedReadSource.cancel()
```

and all of the operations taking place as a part of the hedged read will be canceled!

There's also a performance benefit to cancellation hierarchies: since the requests to each host is only a part of the work performed by the larger hedged-read operation, at least one request will complete well before the entire operation does. Since all of the cancellation callback state for work done by, say, the request to `host1`, is owned by `hostOneSource`, rather than the parent `hedgedReadSource`, it can independently be cleaned up and the relevant memory freed before the entire hedged read operation is complete. For more details, see the comment for the constructor `CancellationSource(const CancellationToken& token)` in [cancellation.h](https://github.com/mongodb/mongo/blob/99d28dd184ada37720d0dae1f3d8c35fec85bd4b/src/mongo/util/cancellation.h#L216-L229).

### Integration With Future Types

`CancellationSources` and `CancellationTokens` integrate neatly with the variety of `Future` types to make it easy to cancel work chained onto `Future` continuations.

#### ExecutorFutures

Integration with `ExecutorFutures` is provided primarily by the `CancelableExecutor` type. If you have some work that you'd like to run on an Executor `exec`, but want to cancel that work if a `CancellationToken` `token` is canceled, you can simply use `CancelableExecutor::make(exec, token)` to get an executor that will run work on `exec` only if `token` has not been canceled when that work is ready to run. As an example, take the following code snippet:

```c++
ExecutorFuture(exec).then([] { doThing1(); })
                    .thenRunOn(CancelableExecutor::make(exec, token))
                    .then([] { doThing2(); })
                    .thenRunOn(exec)
                    .then([] { doThing3();})
                    .onError([](Status) { doThing4(); })
```

In this example, `doThing1()` will run on the executor `exec`; when it has completed, `doThing2()` will run on `exec` only if `token` has not yet been canceled. If `token` wasn't canceled, `doThing3()` will run after `doThing2()`; if `token` was canceled, then the error`CallbackCanceled` will be propagated down the continuation chain until a continuation and executor that accept the error are found (in this case, `doThing4()` will run on `exec`).

#### Future, SemiFuture, and SharedSemiFuture

The primary interface for waiting on futures in a cancelable manner is the free function template:

```c++
template <typename FutureT, typename Value = typename FutureT::value_type>
SemiFuture<Value> future_util::withCancellation(FutureT&& f, const CancellationToken& token);
```

Note that this function also works with executor futures. This function returns a SemiFuture\<T\> that is resolved when either the input future `f` is resolved or the input `CancellationToken token` is canceled - whichever comes first. The returned `SemiFuture` is set with the result of the input future when it resolves first, and with an `ErrorCodes::CallbackCanceled` status if cancellation occurs first.

For example, if we have a `Future<Request> requestFuture` and `CancellationToken token`, and we want to do some work when _either_ `requestFuture` is resolved _or_ `token` is canceled, we can simply do the following:

```c++
future_util::withCancellation(requestFuture, token)
              .then([](Request r) { /* requestFuture was fulfilled; handle it */ })
              .onError<ErrorCodes::CallbackCanceled>([](Status s) { /* handle cancellation */ })
              .onError([](Status s) {/* handle other errors */})
```

### Links to Relevant Code + Example Tests

-   [CancellationSource/CancellationToken implementations](https://github.com/mongodb/mongo/blob/master/src/mongo/util/cancellation.h)
-   [CancellationSource/CancellationToken unit tests](https://github.com/mongodb/mongo/blob/master/src/mongo/util/cancellation_test.cpp)
-   [CancelableExecutor implementation](https://github.com/mongodb/mongo/blob/master/src/mongo/executor/cancelable_executor.h)
-   [CancelableExecutor unit tests](https://github.com/mongodb/mongo/blob/master/src/mongo/executor/cancelable_executor_test.cpp)
-   [future_util::withCancellation implementation](https://github.com/mongodb/mongo/blob/99d28dd184ada37720d0dae1f3d8c35fec85bd4b/src/mongo/util/future_util.h#L658)
-   [future_util::withCancellation unit tests](https://github.com/mongodb/mongo/blob/99d28dd184ada37720d0dae1f3d8c35fec85bd4b/src/mongo/util/future_util_test.cpp#L1268-L1343)
