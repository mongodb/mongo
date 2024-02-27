# Futures and Promises

[Futures][future] are a programming construct that represent deferred values: that is, values that
may not be available until some point in the future. Futures make it easier to structure our
programs asynchronously. If some unit of code (subsystem, function, service, etc) can't produce a
value that has been requested immediately, it can instead return a future that will eventually
contain the value instead of blocking. These future-returning APIs allow threads of execution that
need the results of potentially time-intensive work (like network operations or disk I/O) to
continue performing other work instead of waiting synchronously for those results.

## A Few Definitions

-   A `Future<T>` is a type that will eventually contain either a `T`, or an error indicating why the
    `T` could not be produced (in MongoDB, the error will take the form of either an exception or a
    `Status`).
-   A `Promise<T>` is a single-shot producer of a value (i.e., a `T`) for an associated `Future<T>`.
    That is, to put a value or error in a `Future<T>` and make it ready for use by consumers, the
    value is emplaced in the corresponding `Promise<T>`.
-   A continuation is a functor that can be chained on to `Future<T>` that will execute only once the
    `T` (or error) is available and ready. A continuation in this way can "consume" the produced `T`,
    and handle any errors.

## A First Example

To build some intuition around futures and promises, let's see how they might be used. As an
example, we'll look at how they help us rewrite some slow blocking code into fast, concurrent code.
As a distributed system, MongoDB often needs to send RPCs from one machine to another. A sketch of a
simple, synchronous way of doing so might look like this:

```c++
Message call(Message& toSend) {
  ...
  auto transportSession = getTransportSession();
  // Block until toSend is sent to the network
  auto res = transportSession->sinkMessage(toSend);
  handleErrors(res);
  // Block until we receive a response to the message we sent
  auto swResponse = transportSession->sourceMessage();
  if (swResponse.ok()) {
    return swResponse.getValue();
  }
  ...
}
```

This is fine, but some parts of networking are expensive! `TransportSession::sinkMessage` involves
making expensive system calls to enqueue our message into the kernel's networking stack, and
`TransportSession::sourceMessage` entails waiting for a network round-trip to occur! We don't want
busy worker threads to be forced to wait around to hear back from the kernel for these sorts of
expensive operations. Instead, we'd rather let these threads move on to perform other work, and
handle the response from our expensive networking operations when they're available. Futures and
promises allow us to do this. We can rewrite our example as follows:

```c++
Future<Message> call(Message& toSend) {
  ...
  auto transportSession = getTransportSession();
  return transportSession->asyncSinkMessage(toSend)
     .onError([](Status s) { handleError(s); })
     .then([transportSession]() { return transportSession->asyncSourceMessage(); })
     .onCompletion([&](StatusWith<Response> swr) {
          logResponse(swr);
          return swr;
     });
}
```

First, notice that our calls to `TransportSession::sourceMessage` and
`TransportSession::sinkMessage` have been replaced with calls to asynchronous versions of those
functions. These asynchronous versions are future-returning; they don't block, but also don't return
a result right away. Instead, they return a future that we can chain continuations onto; `then,
onError` and `onCompletion` are all member functions of `Future<T>` that take a callable as argument
and invoke that callable when the chained-to future is ready. Unsurprisingly, continuations chained
with `.then` are run when the future is readied successfully with a `T`, and therefore callables
chained with `.then` should take a `T` as argument. Mirroring this behavior, `.onError`
continuations are run only when the future is readied with an error, and continuations chained this
way take a `Status` as argument which they can inspect to discover the error explaining why a `T`
could not be delivered. Continuations chained with `.onCompletion` are run when the future resolves,
no matter how it completes; callables chained this way receive a `StatusWith<T>` containing either
an error or a `T`.

One essential point here is that multiple continuations can be chained, with each successive
continuation receiving as argument (i.e. "consuming") the result of the previous continuation. This
allows for us to easily structure asynchronous services: each unit of work can be placed in a
continuation, and will be run when all of the inputs for that unit of work are ready, without any
thread blocking and waiting. This is explained in more detail in the "How Are Results Propagated
Down Continuation Chains?" section below.

## Filling In Some Details

The example above hopefully showed us how futures can be used to structure asynchronous programs at
a high level, but we've left out some important details about how they work.

### How Are Futures Fulfilled With Values?

In our example, we looked at how some code that needs to wait for results can use `Future`s to be
written in an asynchronous, performant way. But some thread running elsewhere needs to actually
"fulfill" those futures with a value or error. Threads can fulfull the core "promise" of a
`Future<T>` - that it will eventually contain a `T` or an error - by using the appropriately named
`Promise<T>` type. Every pending `Future<T>` is associated with exactly one corresponding
`Promise<T>` that can be used to ready the `Future<T>`, providing it with a value. (Note that a
`Future<T>` may also be "born ready"/already filled with a value when constructed). The `Future<T>`
can be "made ready" by emplacing a value or error in the associated promise with
`Promise<T>::emplaceValue`, `Promise<T>::setError`, or related helper member functions (see the
[promise class][promise] for the entire API). Promises can be used to set a value in their
associated Futures exactly one time, and must do so before being destroyed (otherwise, the future
will be set with the `ErrorCodes::BrokenPromise` error, which is considered a programmer error and
may crash debug builds of the server in the future).

To create a `Promise` that has a Future, you may use the [`PromiseAndFuture<T>`][pf]
utility type. Upon construction, it contains a created `Promise<T>` and its
corresponding `Future<T>`. The perhaps-familiar `makePromiseFuture<T>` factory
function now simply returns `PromiseAndFuture<T>{}`.

As was previously alluded to, it's
also possible to make a "ready future" - one that has no associated promise and is already filled
with a value or error. These might be useful in cases where the code that produces values in a way
that's normally asynchronous happens to have one available already when a request comes in, and
would like to return it right away. To create such a ready future, use `Future<T>::makeReady()`, or
the helper function [makeReadyFutureWith(Func&& func)][mrfw] which will call the specified `func`
and create a ready `Future` from its returned value.

Lastly, there might be occasions when multiple futures should be fulfilled with the same value, at
the same time. This use case is best served by `SharedPromise` and the associated `SharedSemiFuture`
types. A `SharedPromise` is just like a regular promise, except that emplacing a value or error in
it readies many associated `SharedSemiFuture`s that will all be completed at the same time. You can
extract as many associated `SharedSemiFuture`s as you'd like from a `SharedPromise` by calling its
`getFuture()` member function.

### Where Do Continuations Run?

In our example, we chained continuations onto futures using functions like `Future<T>::then()`, and
explained that the continuations we chained will only be invoked once the future we've chained them
onto is ready. But we haven't yet specified how this continuation is invoked: what thread will
actually do the work of running the continuation, and how will it get scheduled?

In the case of `Future<T>`, the answer to this question is easy. If a `Future<T>` is already ready
when a continuation is being chained to it, then whatever thread is chaining the continuation will
also run it immediately, inline. Otherwise, if the continuation is being chained to an unready
future, whatever thread readies the future by emplacing a value or error in the corresponding
promise will immediately run any continuations chained to it after readying it. However, this
behavior is sometimes undesirable. For example, some service may accept requests for `T`s from
calling threads, and return `Future<T>`s to those threads that will be readied once a `T` is
available. The service may have its own internal threads it uses to produce `T`s, and doesn't want
to lend out its internal threads to do the work chained via continuations to the `Future<T>`s it's
given to calling threads. Instead, it needs to insist that continuations are not chained onto the
futures it gives out, or that the caller receiving the future
arranges for some _other_ thread to run continuations.

Fortunately, the service can enforce these guarantees using two types closely related to
`Future<T>`: the types `SemiFuture<T>` and `ExecutorFuture<T>`.

#### SemiFuture

`SemiFuture`s are like regular futures, except that continuations cannot be chained to them.
Instead, values and errors can only be extracted from them via blocking methods, which threads can
call if they are willing to block. A `Future<T>` can always be transformed into a `SemiFuture<T>`
using the member function `Future<T>::semi()`. Let's look at a quick example to make this clearer:

```c++
// Code producing a `SemiFuture`
SemiFuture<Work> SomeAsyncService::requestWork() {
  PromiseAndFuture<Work> pf;
  _privateExecutor->schedule([promise = std::move(pf.promise)](Status s) {
      if (s.isOK()) {
        auto w = produceWork();
        promise.emplaceValue(w);
      } else {
        // handle error case
      }
  });
  return std::move(pf.future).semi();
}

// Code consuming a `SemiFuture`
SemiFuture<Work> sf = SomeAsyncService::requestWork();
// sf.then(...) wont' compile because sf is a SemiFuture, which doesn't allow chaining continuations
// sf.onError(...) won't compile for the same reason
auto res = sf.get(); // OK; get blocks until sf is ready
```

Our example begins when a thread makes a request for some asynchronous work to be performed by some
service, using `SomeAsyncService::requestWork()`. As was the case in our initial example, this
thread receives back a future that will be readied when its request has been completed and a value
or error is available. However, in this case, the thread receives a `SemiFuture`, instead of a
regular `Future`, which forbids it from chaining continuations to the future via `.then()` and the
like. A quick look into the implementation details of `SomeAsyncService::requestWork()` reveals why:
the service uses its own private executor to do the work that will eventually ready the `SemiFuture`
returned to the caller. If a regular `Future` were returned, and continuations were chained onto it,
the thread from `SomeAsyncService`'s `_privateExecutor` that readied the promise would be forced to
run the continuations. By instead returning a `SemiFuture`, the `SomeAsyncService` prevents the code
that requests work from it from using its own internal `_privateExecutor` resource.

#### ExecutorFuture

`ExecutorFuture`s are another variation on the core `Future` type; they are like regular `Future`s,
except for the fact that code constructing an `ExecutorFuture` is required to provide an
[executor][executor] on which any continuations chained to the future will be run. (An executor is
an abstraction that allows one to schedule code to be run later, in a different execution context.
See the documentation for it [here][executorDocs]). A `SemiFuture` or regular `Future` can be
converted to an `ExecutorFuture` by calling their member function
[thenRunOn(ExecutorPtr)][thenRunOn], which returns an `ExecutorFuture` that allows you to chain
continuations guaranteed to run on the given executor. Again, an example will help make things
clearer, so we'll reuse the one above. Let's imagine the thread that scheduled work by calling
`SomeAsyncService::requestWork()` can't afford to block until the result `SemiFuture` is readied.
Instead, it consumes the asynchronous result by specifying a callback to run and an executor on
which to run it like so:

```c++
// Code consuming a `SemiFuture`
SomeAsyncService::requestWork()              // <-- temporary `SemiFuture`
  .thenRunOn(_executor)                      // <-- Transformed into a `ExecutorFuture`
  .then([](Work w) { doMoreWork(w); }); // <-- Which supports chaining
```

By calling `.thenRunOn(_executor)` on the `SemiFuture` returned by
`SomeAsyncService::requestWork()`, we transform it from a `SemiFuture` to an `ExecutorFuture`. This
allows us to again chain continuations to run when the future is ready, but instead of those
continuations being run on whatever thread readied the future, they will be run on `_executor`. In
this way, the result of the future returned by `SomeAsyncService::requestWork()` is able to be
consumed by the `doMoreWork` function which will run on `_executor`.

### How Are Results Propagated Down Continuation Chains?

In our example for an asyncified `call()` function above, we saw that we could attach continuations
onto futures, like the one returned by `TransportSession::asyncSinkMessage`. We also saw that once
we attached one continuation to a future, we could attach subsequent ones, forming a continuation
chain. In our example, we could say that the continuations chained via `.then()`, `.onError()`, and
`.onCompletion()` form a chain that consumes the result of the future returned by
`TransportSession::asyncSinkMessage`.

Recall that we said a `Future<T>`, when resolved, is guaranteed to either contain a `T` or an error,
in the form of a `Status` or `DBException`. Because a `Future<T>` can resolve to different types in
this way, we can chain different continuations to a `Future<T>` to consume its result, depending on
what the type of the result is (i.e. a `T` or `Status`). We mentioned above that `.then()` is used
to chain continuations that run when the future to which the continuation is chained resolves
successfully. As a result, when a continuation is chained via `.then()` to a `Future<T>`, the
continuation must accept a `T`, the result of the `Future<T>`, as an argument to consume. In the
case of a `Future<void>`, continuations chained via `.then()` accept no arguments. Similarly, as
`.onError()` is used to chain continuations that run when the future is resolved with an error,
these continuations must accept a `Status` as argument, which contains the error the future it is
chained to resolves with. Lastly, as `.onCompletion()` is used to chain continuations that run in
case a `Future<T>` resolves with success or error, continuations chained via this function must
accept an argument that can contain the results of successful resolution of the chained-to future or
an error. When `T` is non-void, continuations chained via `.onCompletion()` must therefore accept a
`StatusWith<T>` as argument, which will contain a `T` if the chained-to future resolved successfully
and an error status otherwise. If `T` is void, a continuation chained via `.onCompletion()` must
accept a `Status` as argument, indicating whether or not the future the continuation is chained to
resolved with success or error.

When a `Future<T>` is resolved, the result (either a `T` or error) will traverse the continuations
chained to the `Future<T>`, in the order they were chained, until it finds the first continuation
that accepts the result of the `Future<T>`. This continuation is then run, and consumes the result
of the input future; it takes this result as an argument and can process it however it wishes. The
continuation can then return a new result, which continues traversing the remainder of the
continuation chain until it finds a continuation that can consume it, in the same way the result of
the input future did. Notably, results will bypass any continuations chained that cannot consume
them, and these continuations will never be run.

Let's take our initial example `call()` from above to see how this works. Let's say the future
returned by `TransportLayer::asyncSinkMessage` resolved successfully. Because it resolved
successfully, there is no error `Status` to report, so the continuation chained via `.onError()`
will be bypassed and will never run. Next, the successful result reaches the continuation chained
via `.then()`, which must take no arguments as `TransportLayer::asyncSinkMessage` returns a
`Future<void>`. Because the future returned by `TransportLayer::asyncSinkMessage` resolved
successfully, the continuation chained via `.then()` does run. The result of this continuation is
the future returned by `TransportLayer::asyncSourceMessage`. When this future resolves, the result
will traverse the remaining continuation chain, and find the continuation chained via
`.onCompletion()`, which always accepts the result of a future, however it resolves, and therefore
is run.

Note that all of the continuation-chaining functions we've discussed, like `.then()`, return future-
like types themselves (i.e. `Future<T>`, `SemiFuture<T>`, and the like). When we chain
continuations in the manner we've been discussing here, subsequent continuations run when the future
returned by the previous continuation is ready, and the future-like type is "unwrapped" such that
the type wrapped by the future (or, in the case of failure, the error) is passed directly to the
subsequent continuation. For more detail on this topic, see the block comment above the
continuation-chaining member functions in [future.h][future], starting above the definition for
`then()`.

At some point, we may have no more continuations to add to a future chain, and will want to either
synchronously extract the value or error held in the last future of the chain, or add a callback to
asynchronously consume this value. The `.get()` and `.getAsync()` members of future-like types
provide these facilities for terminating a future chain by extracting or asynchronouslyunsly
consuming the result of the chain. The `.getAsync()` function works much like `.onCompletion()`,
taking a `Status` or `StatusWith<T>` and running regardless of whether or not the previous link in
the chain resolved with error or success, and running asynchronously when the previous results are
ready (to determine what thread `.getAsync()` will run on, follow the rules laid out in the previous
"Where Do Continuations Run?" section.) Conversely, `.get()` takes no arguments, and blocks when it
is called until the entirety of the continuation chain is resolved, with the final result given back
to the blocking caller. Note that if the final result of the chain was an error that can be
converted to a MongoDB `Status` type (i.e. either a `Status`-family type or `DBException`), it will
be re-thrown as a `DBException` at the site where `.get()` is called when it is available. If the
code calling `.get()` is not capable of handling an exception, use `.getNoThrow()` instead to
extract the same error in the form of a `Status`. In the case of `.getAsync()`, all errors are
converted to `Status`, and crucially, callables chained as continuations via `.getAsync()` cannot
throw any exceptions, as there is no appropriate context with which to handle an asynchronous
exception. If an exception is thrown from a continuation chained via `.getAsync()`, the entire
process will be terminated (i.e. the program will crash).

## Notes and Links

This document is intended as a high-level overview of how to use futures for asynchronous
programming inside the MongoDB server; to understand the complete API of `Promise<T>`, `Future<T>`,
and all the related types, check out the [header file][future] and search for the class or helper
function you're interested in.

### Future Utilities

We have many utilities written to help make it easier for you to work with futures; check out
[future_util.h][future_util.h] to see them. Their [unit tests][utilUnitTests] also help elucidate
how they can be useful. Additionally, when making requests for asynchronous work through future-ful
APIs, you might find you want to cancel that work later on. Cancellation tokens are a concept in the
MongoDB code base that makes doing so easy, and integrate very well with MongoDB future types and
the associated utilities. For more on them, see their architecture guide in [this
README][cancelationArch].

## General Promise/Future Docs

For intro-documentation on programming with promises and futures, this blog post about future use at
[Facebook][fb] and the documentation for the use of promises and futures at [Twitter][twtr] are also
very helpful.

[future]: ../src/mongo/util/future.h
[future_util.h]: ../src/mongo/util/future_util.h
[executor]: ../src/mongo/util/out_of_line_executor.h
[thenRunOn]: ../src/mongo/util/future.h#L250
[promise]: ../src/mongo/util/future.h#L769
[pf]: ../src/mongo/util/future.h#L1156
[mrfw]: ../src/mongo/util/future.h#L1216
[cancelationArch]: ../src/mongo/util/README.md
[utilUnitTests]: ../src/mongo/util/future_util_test.cpp
[fb]: https://engineering.fb.com/2015/06/19/developer-tools/futures-for-c-11-at-facebook/
[twtr]: https://twitter.github.io/finagle/guide/Futures.html
[executorDocs]: ../src/mongo/executor/README.md
