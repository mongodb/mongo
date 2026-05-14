# Design: dropping a view over `system.buckets.*` must not drop the buckets collection

## Symptom

On v8.0 binaries the following sequence on a sharded cluster leaves the
collection in a `MissingLocalCollection` state and silently destroys the
underlying time-series data:

```js
db.adminCommand({shardCollection: "test.realts",
                 timeseries: {timeField: 't', metaField: 'm'},
                 key: {m: 1}});
db.createView("fakets", "system.buckets.realts", []);
db.fakets.drop();   // <-- also drops system.buckets.realts on the shard
db.checkMetadataConsistency();
// -> [{ type: 'MissingLocalCollection',
//      details: { namespace: 'test.system.buckets.realts', ... } }]
```

The sharding catalog still has `system.buckets.realts` registered, but the
local buckets collection on the primary shard is gone, so the time-series
collection becomes unreadable. The companion jstest at
`jstests/sharding/timeseries/timeseries_drop_view_over_system_buckets.js`
reproduces this failure on unpatched code and pins the patched contract.

## Root cause

In `src/mongo/db/catalog/drop_collection.cpp` (v8.0 line range cited in the
ticket, around L532-L535) the `drop` command for a view inspects the
catalog `Collection` object and, when it appears to back a time-series
namespace, also drops `coll->viewOn()`. That field is whatever
namespace the user supplied to `createView`. For an arbitrary user view
whose `viewOn` happens to be `system.buckets.<name>`, the local buckets
collection is destroyed even though the view is not the canonical
time-series view created by `createCollection({timeseries: ...})`.

The sharding DDL coordinator side does the correct thing: it looks up
`system.buckets.fakets`, finds nothing tracked, and does not touch the
global catalog entry for `system.buckets.realts`. The local-shard drop
runs independently and silently desynchronises the two catalogs.

## Proposed fix

Two complementary changes, both small and local to the view-drop path.

1. **Scope the cascading drop to the canonical buckets namespace only.**
   When dropping a view, if the code wants to also drop a buckets
   collection it must use `coll->ns().makeTimeseriesBucketsNamespace()`
   (i.e. `system.buckets.<view-name>`) rather than `coll->viewOn()`. The
   buckets companion of a real time-series view is always
   `system.buckets.<same-name>`; any other `viewOn` value identifies a
   user-defined view that happens to read from a buckets namespace and
   must not be cascaded.

2. **Refuse, at create time, user views whose `viewOn` targets
   `system.buckets.*`.** A view that reads from a buckets namespace is
   a sharp edge with no legitimate user-facing use case in v8.0: the
   buckets schema is internal, exposing it via a hand-rolled view does
   not produce a usable time-series surface, and it is the exact
   configuration that triggers the data-loss path. Returning
   `IllegalOperation` on `createView` when `viewOn` matches
   `system.buckets.*` removes the only known trigger. Existing legit
   time-series views are created by `createCollection`, not
   `createView`, so this change does not affect supported flows.

Change (1) is the load-bearing correctness fix; change (2) is
defense in depth and matches the spirit of the existing
`IllegalOperation` guard against direct drops of
`system.buckets.<name>`.

`v8.1+` already fixes the local-drop branch as part of `SERVER-94829`,
and viewless time-series make the entire class impossible, so the
backport target is `v8.0` only.

## Test plan

* New jstest reproduces the data-loss state on unpatched v8.0 and pins
  all four invariants on patched code: view is gone, buckets collection
  survives on the owning shard, `checkMetadataConsistency()` stays
  clean, and the time-series collection remains readable.
* Existing `jstests/sharding/timeseries/timeseries_drop.js` continues to
  exercise the legitimate buckets-drop path through `coll.drop()` on
  the time-series collection itself; the proposed fix does not alter
  that flow.
* Add a unit-level guard in `drop_collection_test.cpp` if a fixture
  exists for the view-drop path; otherwise the jstest is sufficient
  for a v8.0 backport-only fix.

## Out of scope

* No change to `v8.1+` — already corrected by `SERVER-94829`.
* No change to viewless time-series, which structurally cannot hit this
  bug.
* No change to the sharding DDL coordinator: it already does the right
  thing.
