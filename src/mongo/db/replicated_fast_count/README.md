# Replicated metadata

This subsystem maintains each collection's document count and logical data size (the sum of document
bytes, not storage or index size) without scanning the collection. “Fast count” is the historical
name; the same machinery tracks both size and count.

## Mental model

There are three representations of the metadata:

| Representation                            | Purpose                                                                          |
| ----------------------------------------- | -------------------------------------------------------------------------------- |
| `RecordStore` size/count atomics          | Serve current, committed values in memory.                                       |
| Oplog metadata                            | Record changes in replication order, including transaction commit semantics.     |
| Persisted metadata + checkpoint timestamp | Provide a durable baseline; replay later oplog deltas to recover current values. |

The persisted stores use [containers](../storage/container.h): key/value storage tables addressed
directly by storage ident, rather than catalog-managed collections. We use containers primarily to
keep this internal metadata from being exposed to end users as regular collections. The
[container write API](../collection_crud/container_write.h) still provides oplog replication, and
storage transactions let us update both stores atomically.

The checkpoint path derives its values from the **oplog**, not from the live atomics: concurrent
writes can move the atomics beyond the checkpoint's logical cutoff.

```text
collection write ──> uncommitted deltas ──commit──> RecordStore atomics ──> reads
       │
       └──> oplog (m: size/count metadata)
                  │
                  └──> tailer ──> checkpoint buffer ──> flusher
                                                           │
                                      replicated container writes
                                                           │
                                         metadata + timestamp stores
```

## In-memory accounting: writes and reads

These counter updates and reads operate only on in-memory state. Committing a counter delta does not
write the persisted metadata stores, and reading a counter does not consult those stores or scan the
oplog. Durable accounting follows a separate, oplog-driven path described below.

- **Writes:** [The collection write path](../collection_crud/collection_write_path.cpp) records
  per-UUID changes in [`UncommittedFastCountChanges`](replicated_fast_count_uncommitted_changes.h),
  a `RecoveryUnit::Snapshot` decoration that survives across a multi-document transaction. On
  commit, [`ReplicatedFastCountManager`](replicated_fast_count_manager.cpp) adjusts the associated
  `RecordStore`; aborts do not update the committed atomics.
- **Reads:** [`CollectionImpl`](../shard_role/shard_catalog/collection_impl.cpp) reads the committed
  atomics plus the caller's uncommitted delta. These values reflect the latest committed changes
  plus the caller's own pending changes, rather than the documents visible at a particular read
  timestamp. [Enablement and eligibility check](replicated_fast_count_enabled.cpp) controls tracking
  and read selection: the persistence provider can require replicated fast count, otherwise
  feature-flag/FCV checks apply. Most local and implicitly replicated collections are excluded. The
  oplog is an exception for persistence providers that require replicated fast count; the
  feature-flag-only read path excludes it.

## Durable accounting: oplog and logical checkpoints

Separately, [`OpObserverImpl`](../op_observer/op_observer_impl.cpp) writes the oplog's `m` field
using [`durable_size_metadata.idl`](durable_size_metadata.idl). For a single CRUD operation, `sz` is
the size delta; count is inferred as insert `+1`, delete `-1`, update `0`. Missing `sz` opts out of
size/count tracking. Prepared transactions instead carry per-UUID totals on `commitTransaction`;
unprepared transactions carry metadata on the inner `applyOps` operations.

[`SizeCountCheckpointCoordinator`](size_count_checkpoint_coordinator.cpp) owns two primary-only
threads, with a lifetime of one primary term:

1. The [tailer](size_count_checkpoint_oplog_tailer.cpp) scans newly visible, **no-holes** oplog
   entries in order. The
   [streaming accumulator](replicated_fast_count_streaming_oplog_delta_accumulator.h) combines
   per-UUID deltas, tracks create/drop state, and withholds transaction changes until commit.
2. A flush request (including the storage `FlushAllFilesObserver` hook) causes the
   [flusher](size_count_checkpoint_flusher.cpp) to write a no-op **watermark** at timestamp `T`. The
   [buffer](size_count_checkpoint_buffer.h) cuts an in-flight batch when the tailer reaches the
   watermark; tailing can continue into a separate pending batch while persistence runs.
3. The flusher adds the batch's deltas to persisted totals and atomically writes updated metadata
   rows and the global checkpoint timestamp. These container writes are themselves replicated.

```text
oplog:  ... user writes ... [watermark T] ... [metadata + timestamp writes at T' > T] ...
```

The checkpoint's **valid-as-of** timestamp is `T`, not `T'`. [`SizeCountStore`](size_count_store.h)
persists each UUID's size, count, and valid-as-of timestamp;
[`SizeCountTimestampStore`](size_count_timestamp_store.h) records the valid-as-of timestamp of the
latest persisted metadata checkpoint. Unchanged rows need not be rewritten. The size and count of
`SizeCountStore` and `SizeCountTimestampStore` themselves are not tracked, so internal metadata
writes do not contribute collection deltas or advance the checkpoint's valid-as-of timestamp. This
prevents checkpoint persistence from generating further checkpoint work. For the oplog's own
size/count, the accumulator counts the raw bytes and number of entries—including internal metadata
writes—rather than using their `m` fields. A failed flush retains the in-flight batch for retry;
acknowledgement clears it only after a successful flush or a no-op.

The persisted valid-as-of timestamp also constrains
[oplog truncation](../storage/oplog_truncation.cpp): the oplog must retain at least one entry at or
before that timestamp, plus all subsequent entries, so later metadata changes remain available.
Until the first checkpoint is persisted, truncation is blocked.

A small, schematic flush entry (unrelated fields/rows omitted, UUID key abbreviated, numeric types
simplified, and binary `v` values shown as decoded BSON):

```javascript
{
  ts: Timestamp(100, 2), op: "c", ns: "admin.$cmd",
  o: { applyOps: [
    { op: "ci", ns: "admin.$container", container: "internal-fastCountMetadataStore",
      o: { k: "<collection UUID bytes>",
           v: { "valid-as-of": Timestamp(100, 1), meta: { sz: 30, ct: 2 } } } },
    { op: "ci", ns: "admin.$container", container: "internal-fastCountMetadataStoreTimestamps",
      o: { k: 0, v: { "valid-as-of": Timestamp(100, 1) } } }
  ] }
}
```

Here `ci` inserts new rows: the collection's absolute totals are 30 bytes / 2 documents as of
`T = Timestamp(100, 1)`. The totals and the checkpoint's valid-as-of timestamp are written together
at `T' = Timestamp(100, 2)`.

## Recovery and replication

- **Startup:** `ReplicatedFastCountManager::initializeMetadata()` loads the persisted baseline, adds
  oplog deltas strictly after the global valid-as-of timestamp, then installs the results in catalog
  collections' `RecordStore`s. Correct reconstruction requires the intervening oplog history.
- **Step-up / step-down:** [setup](replicated_fast_count_init.cpp) opens the two internal containers
  and starts checkpointing from the persisted boundary. Step-down interrupts worker operation
  contexts before joining the threads; a new term gets a new coordinator.

## Validation and maintenance

[`validate`](../validate/collection_validation.cpp) checks replicated metadata;
[`repairReplicatedMetadata`](repair_replicated_metadata_command.cpp) emits replicated corrections
rather than patching only the local cache. Diagnostics live under `serverStatus.replicatedFastCount`
([metrics](replicated_fast_count_metrics.cpp)), including thread liveness, failures/retries, and
checkpoint lag. Tests alongside each component cover its invariants; start with
[`replicated_fast_count_test.cpp`](replicated_fast_count_test.cpp) and
[`replicated_fast_count_txn_test.cpp`](replicated_fast_count_txn_test.cpp) for end-to-end unit
coverage.
