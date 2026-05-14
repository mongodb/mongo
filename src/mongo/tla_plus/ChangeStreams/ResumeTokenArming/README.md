# ResumeTokenArming

TLA+ specification of the change-stream **lazy cursor arming** hazard
and its `startAtOperationTime` fix.

## The hazard

A client opens a change-stream cursor without `startAtOperationTime`:

```js
const cs = db.coll.watch(pipeline);  // <-- no startAtOperationTime
for await (const change of cs) { /* ... */ }
```

The documented contract says the cursor starts at "the time the cursor
was opened on the server". Some drivers implement `watch()` lazily:
the call returns to the caller without performing a server round-trip,
and the cursor is only registered on the server when the iterator
issues its first `getMore`. Under that implementation the server-side
open time is the first-getMore time, not the time `watch()` returned.
Any change with cluster timestamp in

```
[watch_returned_to_caller, server_received_first_getMore)
```

is silently dropped.

## The fix

Capture cluster time first via `hello`, then pin the cursor:

```js
const { operationTime } = await db.command({ hello: 1 });
const cs = db.coll.watch(pipeline, { startAtOperationTime: operationTime });
```

The server registers the cursor at `operationTime`, not at first
`getMore`. The open window vanishes.

## The spec

`ResumeTokenArming.tla` models a writer, a consumer, an oplog, and a
cursor with two open modes (lazy / pinned), parameterized by
`UseStartAt`. The key invariant is `NoEventLoss`: once the cursor has
drained, every oplog entry whose timestamp is at least the consumer's
captured `hello_ts` must be in `delivered`.

* `UseStartAt = FALSE`: TLC reports a `NoEventLoss` violation with a
  short counterexample where `WriterEmit` fires between
  `ConsumerOpenCursorWithoutStartAt` and `ConsumerGetMore`.
* `UseStartAt = TRUE`: TLC completes the full reachable state space
  with no invariant violation.

## Run

```sh
cd src/mongo/tla_plus
./download-tlc.sh             # one-time
./model-check.sh ChangeStreams/ResumeTokenArming
```

To verify the fix, set `UseStartAt = TRUE` in
`MCResumeTokenArming.cfg` and re-run.
