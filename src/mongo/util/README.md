# MongoDB Utilities and Primatives

This document provides a high-level overview of certain utilities and primatives used within the
server. This is a work in progress and more sections will be added gradually.

## Fail Points

For details on the server-internal *FailPoint* pattern, see [this document][fail_points].

[fail_points]: ../../../docs/fail_points.md

## Cancelation Sources and Tokens

### Intro
When writing asynchronous code, we often schedule code or operations to run at some point in the future, in a different execution context. Sometimes, we want to cancel that scheduled work - to stop it from ever running if it hasn't yet run, and possibly to interrupt its execution if it is safe to do so. For example, in the MongoDB server, we might want to:
 - Cancel work scheduled on executors
 - Cancel asynchronous work chained as continuations on futures
 - Write and use services that asynchronously perform cancelable work for consumers in the background
 
In the MongoDB server, we have two types that together make it easy to manage the cancelation of this sort of asynchronous work: CancelationSources and CancelationTokens.

### The CancelationSource and CancelationToken Types
A `CancelationSource` manages the cancelation state for some unit of asynchronous work. This unit of asynchronous work can consist of any number of cancelable operations that should all be canceled together, e.g. functions scheduled to run on an executor, continuations on futures, or operations run by a service implementing cancelation. A `CancelationSource` is constructed in an uncanceled state, and cancelation can be requested by calling the member `CancelationSource::cancel()`.
 
A `CancelationSource` can be used to produce associated CancelationTokens with the member function `CancelationSource::token()`. These CancelationTokens can be passed to asynchronous operations to make them cancelable. By accepting a `CancelationToken` as a parameter, an asynchronous operation signals that it will attempt to cancel the operation when the `CancelationSource` associated with that `CancelationToken` has been canceled.

When passed a `CancelationToken`, asynchronous operations are able to handle the cancelation of the `CancelationSource` associated with that `CancelationToken` in two ways:

 - The `CancelationToken::isCanceled()` member function can be used to check at any point in time if the `CancelationSource` the `CancelationToken` was obtained from has been canceled. The code implementing the asynchronous operation can therefore check the value of this member function at appropriate points and, if the `CancelationSource` has been canceled, refuse to run the work or stop running work if it is ongoing.

 - The `CancelationToken:onCancel()` member function returns a `SemiFuture` that will be resolved successfully when the underlying `CancelationSource` has been canceled or resolved with an error if it is destructed before being canceled. Continuations can therefore be chained on this future that will run when the associated `CancelationSource` has been canceled. Importantly, because `CancelationToken:onCancel()` returns a `SemiFuture`, implementors of asynchronous operations must provide an execution context in which they want their chained continuation to run.  Normally, this continuation should be scheduled to run on an executor, by passing one to `SemiFuture::thenRunOn()`.  

   - Alternatively, the continuation can be forced to run inline by transforming the `SemiFuture` into an inline future, by using `SemiFuture::unsafeToInlineFuture()`.  This should be used very cautiously.  When a continuation is chained to the `CancelationToken:onCancel()` future via `SemiFuture::unsafeToInlineFuture()`, the thread that calls `CancelationSource::cancel()` will be forced to run the continuation inline when it makes that call. Note that this means if a service chains many continuations in this way on `CancelationToken`s obtained from the same `CancelationSource`, then whatever thread calls`CancelationSource::cancel()` on that source will be forced to run all of those continuations potentially blocking that thread from making further progress for a non-trivial amount of time.  Do not use `SemiFuture::unsafeToInlineFuture()` in this way unless you are sure you can block the thread that cancels the underlying `CancelationSource` until cancelation is complete.  Additionally, remember that because the  `SemiFuture` returned by `CancelationToken::onCancel()` is resolved as soon as that `CancelationToken` is canceled, if you attempt to chain a continuation on that future when the `CancelationToken` has _already_ been canceled, that continuation will be ready to run right away. Ordinarily, this just means the continuation will immediately be scheduled on the provided executor, but if `SemiFuture::unsafeToInlineFuture` is used to force the continuation to run inline, it will run inline immediately, potentially leading to deadlocks if you're not careful.

### Example of a Service Performing Cancelable, Asynchronous Work
We'll use the WaitForMajorityService as an example of how work can be scheduled and cancelled using CancelationSources and CancelationTokens. First, we'll see how a consumer might use a service implementing cancelation. Then, we'll see how a service might implement cancelation internally.

#### Using a Cancelable Service 
The WaitForMajorityService allows consumers to asynchronously wait until an `opTime` is majority committed. Consumers can request a wait on a specific `opTime` by calling the function `WaitForMajorityService::waitUntilMajority(OpTime opTime, CancelationToken token)`. This call will return a future that will be resolved when that `opTime` has been majority committed or otherwise set with an error.

In some cases, though, a consumer might realize that it no longer wants to wait on an `opTime`. For example, the consumer might be going through shut-down, and it needs to clean up all of its resources cleanly right away. Or, it might just realize that the `opTime` is no longer relevant, and it would like to tell the `WaitForMajorityService` that it no longer needs to wait on it, so the `WaitForMajorityService` can conserve its own resources. 

Consumers can easily cancel existing requests to wait on `opTime`s in situations like these by making use of the `CancelationToken` argument accepted by `WaitForMajorityService::waitUntilMajority`. For any `opTime` waits that should be cancelled together, they simply pass `CancelationTokens` from the same `CancelationSource` into the requests to wait on those `opTimes`:

```c++
CancelationSource source;
auto opTimeFuture = WaitForMajorityService::waitUntilMajority(opTime, source.token());
auto opTimeFuture2 = WaitForMajorityService::waitUntilMajority(opTime2, source.token());
```

And whenever they want to cancel the waits on those `opTime`s, they can simply call:

```c++
source.cancel()
```
After this call, the `WaitForMajorityService` will stop waiting on `opTime` and `opTime2`. And the futures returned by all calls to `waitUntilMajority` that were passed `CancelationToken`s from the `CancelationSource source` (in this case, `opTimeFuture` and `opTimeFuture2`) will be resolved with `ErrorCodes::CallbackCanceled`.

#### Implementing a Cancelable Service
Now we'll see how `WaitForMajorityService` might be implemented, at a high level, to support the cancelable API we saw in the last section. The `WaitForMajorityService` will need to ensure that calls to `WaitForMajorityService::waitUntilMajority(opTime, token)` schedule work to wait until `opTime` is majority committed, and that this scheduled work will stop if `token` has been canceled. To do so, it can use either of the functions `CancelationToken` has that expose the underlying cancelation state: it can either call `CancelationToken::isCanceled()` at some appropriate time on `token` and conditionally stop waiting on `opTime`, or it can chain a continuation onto the future returned by `CancelationToken:onCancel()` that will stop the wait. This continuation will run when the `token` is canceled, as cancelation resolves the future returned by `CancelationToken::onCancel()`.

To keep the example general, we're going to elide some details: for now, assume that calling `stopWaiting(opTime)` performs all the work needed for the `WaitForMajorityService` to stop waiting on `opTime`. Additionally, assume that `opTimePromise` is the promise that resolves the future returned by the call to `waitUntilMajority(opTime, token)`. Then, to implement cancelation for some request to wait on `opTime` with an associated token `token`, the `WaitForMajorityService` can add something like the following code to the function it uses to accept requests:

``` c++
SemiFuture<void> WaitForMajorityService::waitUntilMajority(OpTime opTime, CancelationToken token) {
    // ... Create request that will be processed by a background thread

    token.onCancel().thenRunOn(executor).getAsync([](Status s) {
        if (s.isOK()) { // The token was canceled.
            stopWaiting(opTime);
            opTimePromise.setError({ErrorCodes::CallbackCanceled, "waitUntilMajority canceled"});
        }
    });
}
```
Whenever `token` is canceled, the continuation above will run, which will stop the `WaitForMajorityService` from waiting on `opTime`, and resolve the future originally returned from the call to `waitUntilMajority(opTime, token)` with an error. There's just one more detail -- we don't want the cancelation and ordinary completion of the work to race. If `token` is canceled _after_ we've finished waiting for opTime to be majority committed, there's no work to cancel, and we can't set opTimePromise twice! To fix this, we can simply protect opTimePromise with an atomic, ensuring that it will be set exactly once. Then, before we perform either cancelation work or fulfilling the promise by ordinary means, we can use the atomic to check that the promise has not already been completed. This is the gist of what it takes to write a service performing cancelable work! To see the full details of making a cancelable `WaitForMajorityService`, see this [commit](https://github.com/mongodb/mongo/commit/4fa2fcb16107c860448b58cd66798bae140e7263).

### Cancelation Hierarchies
In the above example, we saw how we can use a single `CancelationSource` and associated tokens to cancel work. This works well when we can associate a specific group of asynchronous tasks with a single `CancelationSource`. Sometimes, we may want sub-tasks of a larger operation to be individually cancelable, but also to be able to cancel _all_ tasks associated with the larger operation at once. CancelationSources can be managed hierarchically to make this sort of situation manageable. A hierarchy between CancelationSources is created by passing a `CancelationToken` associated with one `CancelationSource` to the constructor of a newly-created `CancelationSource` as follows:

```c++
CancelationSource parentSource;
CancelationSource childSource(parentSource.token());
```
As the naming suggests, we say that the `parentSource` and `childSource` `CancelationSources` are in a hierarchy, with `parentSource` higher up in the hierarchy. When a `CancelationSource` higher up in a cancelation hierarchy is canceled, all descendant `CancelationSources` are automatically canceled as well. Conversely, the `CancelationSources` lower down in a cancelation hierarchy can be canceled without affecting any other `CancelationSources` higher up or at the same level of the hierarchy.

As an example of this sort of hierarchy of cancelable operations, let's consider the case of [hedged reads](https://docs.mongodb.com/manual/core/read-preference-hedge-option/). Note that hedged reads do not currently use `CancelationTokens` in their implementation; this is for example purposes only. When a query is specified with the "hedged read" option on a sharded cluster, mongos will route the read to two replica set members for each shard targeted by the query return the first response it receives per-shard. Therefore, as soon as it receives a response from one replica set member on a shard, it can cancel the other request it made to a different member on the same shard. We can use the pattern discussed above to do this sort of cancelation. At a high level, the code might look something like this:
    
```c++
// Assuming we already have a CancelationSource called hedgedReadSource for the entire
// hedged-read operation, we create child CancelationSources used to manage the cancelation
// state for each request to a replica set member of the shard
CancelationSource hostOneSource(hedgedReadSource.token());
CancelationSource hostTwoSource(hedgedReadSource.token());

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
We can see the utility of the hierarchy of `CancelationSources` by examining the case where the client indicates that it would like to kill the entire hedged read operation. Rather than having to track every `CancelationSource` used to manage different requests performed throughout the operation, we can call

```c++
hedgedReadSource.cancel()
```

and all of the operations taking place as a part of the hedged read will be canceled!

There's also a performance benefit to cancelation hierarchies: since the requests to each host is only a part of the work performed by the larger hedged-read operation, at least one request will complete well before the entire operation does. Since all of the cancelation callback state for work done by, say, the request to `host1`, is owned by `hostOneSource`, rather than the parent `hedgedReadSource`, it can independently be cleaned up and the relevant memory freed before the entire hedged read operation is complete. For more details, see the comment for the constructor `CancelationSource(const CancelationToken& token)` in [cancelation.h](https://github.com/mongodb/mongo/blob/99d28dd184ada37720d0dae1f3d8c35fec85bd4b/src/mongo/util/cancelation.h#L216-L229).

### Integration With Future Types
`CancelationSources` and `CancelationTokens` integrate neatly with the variety of `Future` types to make it easy to cancel work chained onto `Future` continuations.

#### ExecutorFutures
Integration with `ExecutorFutures` is provided primarily by the `CancelableExecutor` type. If you have some work that you'd like to run on an Executor `exec`, but want to cancel that work if a `CancelationToken` `token` is canceled, you can simply use `CancelableExecutor::make(exec, token)` to get an executor that will run work on `exec` only if `token` has not been canceled when that work is ready to run. As an example, take the following code snippet:
```c++
ExecutorFuture(exec).then([] { doThing1(); })
                    .thenRunOn(CancelableExecutor::make(exec, token))
                    .then([] { doThing2(); }) 
                    .thenRunOn(exec)
                    .then([] { doThing3();})
                    .onError([](Status) { doThing4(); })
```
In this example, `doThing1()` will run on the executor `exec`; when it has completed, `doThing2()` will run on `exec` only if `token` has not yet been canceled.  If `token` wasn't canceled, `doThing3()` will run after `doThing2()`; if `token` was canceled, then the error`CallbackCanceled` will be propagated down the continuation chain until a continuation and executor that accept the error are found (in this case, `doThing4()` will run on `exec`).

#### Future, SemiFuture, and SharedSemiFuture
The primary interface for waiting on futures in a cancelable manner is the free function template:
```c++
template <typename FutureT, typename Value = typename FutureT::value_type>
SemiFuture<Value> future_util::withCancelation(FutureT&& f, const CancelationToken& token);
```
Note that this function also works with executor futures. This function returns a SemiFuture<T> that is resolved when either the input future `f` is resolved or the input `CancelationToken token` is canceled - whichever comes first. The returned `SemiFuture` is set with the result of the input future when it resolves first, and with an `ErrorCodes::CallbackCanceled` status if cancelation occurs first.

For example, if we have a `Future<Request> requestFuture` and `CancelationToken token`, and we want to do some work when _either_ `requestFuture` is resolved _or_ `token` is canceled, we can simply do the following:
```c++
future_util::withCancelation(requestFuture, token)
              .then([](Request r) { /* requestFuture was fulfilled; handle it */ })
              .onError<ErrorCodes::CallbackCanceled>([](Status s) { /* handle cancelation */ })
              .onError([](Status s) {/* handle other errors */})
 ```

### Links to Relevant Code + Example Tests
- [CancelationSource/CancelationToken implementations](https://github.com/mongodb/mongo/blob/master/src/mongo/util/cancelation.h)
- [CancelationSource/CancelationToken unit tests](https://github.com/mongodb/mongo/blob/master/src/mongo/util/cancelation_test.cpp)
- [CancelableExecutor implementation](https://github.com/mongodb/mongo/blob/master/src/mongo/executor/cancelable_executor.h)
- [CancelableExecutor unit tests](https://github.com/mongodb/mongo/blob/master/src/mongo/executor/cancelable_executor_test.cpp)
- [future_util::withCancelation implementation](https://github.com/mongodb/mongo/blob/99d28dd184ada37720d0dae1f3d8c35fec85bd4b/src/mongo/util/future_util.h#L658)
- [future_util::withCancelation unit tests](https://github.com/mongodb/mongo/blob/99d28dd184ada37720d0dae1f3d8c35fec85bd4b/src/mongo/util/future_util_test.cpp#L1268-L1343)
