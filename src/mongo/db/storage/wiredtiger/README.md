# WiredTiger Storage Engine Integration

## Collection and Index to Table relationship

Creating a collection (record store) or index requires two WT operations that cannot be made
atomic/transactional. A WT table must be created with
[WT_SESSION::create](https://source.wiredtiger.com/develop/struct_w_t___s_e_s_s_i_o_n.html#a358ca4141d59c345f401c58501276bbb "WiredTiger Docs") and an insert/update must be made in the \_mdb_catalog table (MongoDB's
catalog). MongoDB orders these as such:

1. Create the WT table
1. Update \_mdb_catalog to reference the table

Note that if the process crashes in between those steps, the collection/index creation never
succeeded. Upon a restart, the WT table is dangling and can be safely deleted.

Dropping a collection/index follows the same pattern, but in reverse.

1. Delete the table from the \_mdb_catalog
1. [Drop the WT table](https://source.wiredtiger.com/develop/struct_w_t___s_e_s_s_i_o_n.html#adf785ef53c16d9dcc77e22cc04c87b70 "WiredTiger Docs")

In this case, if a crash happens between these steps and the change to the \_mdb_catalog was made
durable (in modern versions, only possible via a checkpoint; the \_mdb_catalog is not logged), the
WT table is once again dangling on restart. Note that in the absence of a history, this state is
indistinguishable from the creation case, establishing a strong invariant.

# Checkpoints

The WiredTiger storage engine [supports
checkpoints](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/wiredtiger/wiredtiger_kv_engine.cpp#L443-L647)
, which are a read-only, static view of one or more data sources. When WiredTiger takes a
checkpoint, it writes all of the data in a snapshot to the disk in a consistent way across all of
the data files.

To avoid taking unnecessary checkpoints on an idle server, WiredTiger will only take checkpoints for
the following scenarios:

- When the [stable timestamp](../../repl/README.md#replication-timestamp-glossary) is greater than or
  equal to the [initial data timestamp](../../repl/README.md#replication-timestamp-glossary), we take a
  stable checkpoint, which is a durable view of the data at a particular timestamp. This is for
  steady-state replication.
- The [initial data timestamp](../../repl/README.md#replication-timestamp-glossary) is not set, so we
  must take a full checkpoint. This is when there is no consistent view of the data, such as during
  initial sync.

Not only does checkpointing provide us with durability for the database, but it also enables us to
take [backups of the data](../../storage/README.md#file-system-backups).

When WiredTiger takes a checkpoint, it uses the
[`stable_timestamp`](https://github.com/mongodb/mongo/blob/87de9a0cb1/src/mongo/db/storage/wiredtiger/wiredtiger_kv_engine.cpp#L2011 "Github") (effectively a `read_timestamp`) for what data should be persisted in the checkpoint.
Every "data write" (collection/index contents, \_mdb_catalog contents) corresponding to an oplog
entry with a timestamp <= the `stable_timestamp` will be included in this checkpoint. None of the
data writes later than the `stable_timestamp` are included in the checkpoint. When the checkpoint is
completed, the `stable_timestamp` is known as the checkpoint's
[`checkpoint_timestamp`](https://github.com/mongodb/mongo/blob/834a3c49d9ea9bfe2361650475158fc0dbb374cd/src/third_party/wiredtiger/src/meta/meta_ckpt.c#L921 "Github"). When WiredTiger starts up on a checkpoint, that checkpoint's timestamp is known as the
[`recovery_timestamp`](https://github.com/mongodb/mongo/blob/87de9a0cb1/src/mongo/db/storage/wiredtiger/wiredtiger_kv_engine.cpp#L684 "Github"). The recovery timestamp is used for Replication startup recovery.

# Journaling

WiredTiger journals any collection or index with `log=(enabled=true)` specified at creation. Such
collection and index tables are specially logged / journaled to disk when requested. The MongoDB
change log stored in the oplog collection is journaled, along with most non-replicated `local`
database collections, when the server instance is started with `--replSet`. In standalone mode,
however, MongoDB does not create the `local.oplog.rs` collection and all collections are journaled.

Code links:

- [_Code that ultimately calls flush journal on
  WiredTiger_](https://github.com/mongodb/mongo/blob/767494374cf12d76fc74911d1d0fcc2bbce0cd6b/src/mongo/db/storage/wiredtiger/wiredtiger_session_cache.cpp#L241-L362)
  - Skips flushing if ephemeral mode engine; may do a journal flush or take a checkpoint depending
    on server settings.
- [_Control of whether journaling is
  enabled_](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h#L451)
  - 'durable' confusingly means journaling is enabled.
- [_Whether WT journals a
  collection_](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/wiredtiger/wiredtiger_util.cpp#L560-L580)

# Startup Recovery

During startup, WiredTiger will replay the write-ahead log (journal) entries, if any, from a crash.
In WiredTiger, the write-ahead log also contains entries that are specific to WiredTiger, most of
its entries are to re-insert items into MongoDB's oplog collection.

## Rollback to Stable

Rollback-to-stable is an operation that retains only modifications that are considered stable. In other words, we are rolling back to the latest checkpoint.

The Replication and Storage Engine integration layers kill all user operations and all internal
threads that could access the storage engine. This is necessary because
`WT_CONNECTION::rollback_to_stable` requires all open cursors to be closed or reset, otherwise
`EBUSY` will be returned. In the server we retry on `EBUSY` until the system quiesces.

Once the system is quiesced, Replication and Storage Engine integration layers prevent new
operations from starting. The in-memory representation of the catalog is cleared and the drop
pending state is cleared in the ident reaper as drops may be rolled back. At this point
`WT_CONNECTION::rollback_to_stable` is called. Once we return from this function, the reverse order
of operations is performed. Such as rebuilding the in-memory representation of the catalog, internal
threads are restarted, and two-phase index builds are resumed.

See [here](https://source.wiredtiger.com/develop/arch-rts.html) for WiredTiger's architecture guide
on rollback-to-stable.

See [here](../../repl/README.md#rollback-recover-to-a-timestamp-rtt) for more information on what
happens in the replication layer during rollback-to-stable.

## Repair

Data corruption has a variety of causes, but can usually be attributed to misconfigured or
unreliable I/O subsystems that do not make data durable when called upon, often in the event of
power outages.

MongoDB provides a command-line `--repair` utility that attempts to recover as much data as possible
from an installation that fails to start up due to data corruption.

### Types of Corruption

MongoDB repair attempts to address the following forms of corruption:

- Corrupt WiredTiger data files
  - Includes all collections, `_mdb_catalog`, and `sizeStorer`
- Missing WiredTiger data files
  - Includes all collections, `_mdb_catalog`, and `sizeStorer`
- Index inconsistencies
  - Validate [repair mode](../../validate/README.md#repair-mode) attempts to fix index inconsistencies to avoid a full index
    rebuild.
  - Indexes are rebuilt on collections after they have been salvaged or if they fail validation and
    validate repair mode is unable to fix all errors.
- Un-salvageable collection data files
- Corrupt metadata
  - `WiredTiger.wt`, `WiredTiger.turtle`, and WT journal files
- “Orphaned” data files
  - Collection files missing from the `WiredTiger.wt` metadata
  - Collection files missing from the `_mdb_catalog` table
  - We cannot support restoring orphaned files that are missing from both metadata sources
- Missing `featureCompatibilityVersion` document

### Repair Procedure

1. Initialize the WiredTigerKVEngine. If a call to `wiredtiger_open` returns the `WT_TRY_SALVAGE`
   error code, this indicates there is some form of corruption in the WiredTiger metadata. Attempt
   to [salvage the
   metadata](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/wiredtiger/wiredtiger_kv_engine.cpp#L1046-L1071)
   by using the WiredTiger `salvage=true` configuration option.
2. Initialize the StorageEngine and [salvage the `_mdb_catalog` table, if
   needed](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/storage_engine_impl.cpp#L95).
3. Recover orphaned collections.
   - If an [ident](#glossary) is known to WiredTiger but is not present in the `_mdb_catalog`,
     [create a new
     collection](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/storage_engine_impl.cpp#L145-L189)
     with the prefix `local.orphan.<ident-name>` that references this ident.
   - If an ident is present in the `_mdb_catalog` but not known to WiredTiger, [attempt to recover
     the
     ident](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/storage_engine_impl.cpp#L197-L229).
     This [procedure for orphan
     recovery](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/wiredtiger/wiredtiger_kv_engine.cpp#L1525-L1605)
     is a less reliable and more invasive. It involves moving the corrupt data file to a temporary
     file, creates a new table with the same name, replaces the original data file over the new one,
     and
     [salvages](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/wiredtiger/wiredtiger_kv_engine.cpp#L1525-L1605)
     the table in attempt to reconstruct the table.
4. [Verify collection data
   files](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/wiredtiger/wiredtiger_kv_engine.cpp#L1195-L1226),
   and salvage if necessary.
   - If call to WiredTiger
     [verify()](https://source.wiredtiger.com/develop/struct_w_t___s_e_s_s_i_o_n.html#a0334da4c85fe8af4197c9a7de27467d3)
     fails, call
     [salvage()](https://source.wiredtiger.com/develop/struct_w_t___s_e_s_s_i_o_n.html#ab3399430e474f7005bd5ea20e6ec7a8e),
     which recovers as much data from a WT data file as possible.
   - If a salvage is unsuccessful, rename the data file with a `.corrupt` suffix.
   - If a data file is missing or a salvage was unsuccessful, [drop the original table from the
     metadata, and create a new, empty
     table](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/wiredtiger/wiredtiger_kv_engine.cpp#L1262-L1274)
     under the original name. This allows MongoDB to continue to start up despite present
     corruption.
   - After any salvage operation, [all indexes are
     rebuilt](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/repair_database.cpp#L134-L149)
     for that collection.
5. Validate collection and index consistency
   - [Collection validation](#collection-validation) checks for consistency between the collection
     and indexes. Validate repair mode attempts to fix any inconsistencies it finds.
6. Rebuild indexes
   - If a collection's data has been salvaged or any index inconsistencies are not repairable by
     validate repair mode, [all indexes are
     rebuilt](https://github.com/mongodb/mongo/blob/4406491b2b137984c2583db98068b7d18ea32171/src/mongo/db/repair.cpp#L273-L275).
   - While a unique index is being rebuilt, if any documents are found to have duplicate keys, then
     those documents are inserted into a lost and found collection with the format
     `local.lost_and_found.<collection UUID>`.
7. [Invalidate the replica set
   configuration](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/repair_database_and_check_version.cpp#L460-L485)
   if data has been or could have been modified. This [prevents a repaired node from
   joining](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/repl/replication_coordinator_impl.cpp#L486-L494)
   and threatening the consistency of its replica set.

Additionally:

- When repair starts, it creates a temporary file, `_repair_incomplete` that is only removed when
  repair completes. The server [will not start up
  normally](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/storage/storage_engine_init.cpp#L82-L86)
  as long as this file is present.
- Repair [will restore a
  missing](https://github.com/mongodb/mongo/blob/r4.5.0/src/mongo/db/repair_database_and_check_version.cpp#L434)
  `featureCompatibilityVersion` document in the `admin.system.version` to the lower FCV version
  available.

# Oplog Truncation

The oplog collection can be truncated both at the front end (most recent entries) and the back end
(the oldest entries). The capped setting on the oplog collection causes the oldest oplog entries to
be deleted when new writes increase the collection size past the cap. MongoDB using the WiredTiger
storage engine with `--replSet` handles oplog collection deletion specially via
OplogTruncateMarkers, an oplog specific implementation of the
[CollectionTruncateMarkers](../README.md#collectionTruncateMarkers) mechanism, ignoring the generic capped
collection deletion mechanism. The front of the oplog may be truncated back to a particular
timestamp during replication startup recovery or replication rollback.

A new truncate marker is created when the in-progress marker segment contains more than the minimum
bytes needed to complete the segment; and the oldest truncate marker's oplog is deleted when the
oplog size exceeds its cap size setting.

Oplog sampling operates in two modes: asynchronous and synchronous. By default, it uses asynchronous mode, where oplog sampling and initial marker generation happen in the background as part of the OplogCapMaintainer thread. This setup ensures that startup is not blocked and oplog reads and writes are available during tartup. Until the initial marker generation is complete, no new truncate markers can be created for these new oplog writes.

Asynchronous mode offers better performance, allowing faster startups and restarts while improving overall node availability. One potential trade-off to consider is the possible increased disk usage.

You can switch between asynchronous and synchronous modes by adjusting the OplogSamplingAsyncEnabled server parameter.

Oplog sampling and marker generation is skipped when using `--restore` or `--magicRestore`.

## Special Timestamps That Will Not Be Truncated

The WiredTiger integration layer's `OplogTruncateMarkers` implementation will stall deletion waiting
for certain significant tracked timestamps to move forward past entries in the oldest truncate
marker. This is done for correctness. Backup pins truncation in order to maintain a consistent view
of the oplog; and startup recovery after an unclean shutdown and rollback both require oplog history
back to certain timestamps.

## Min Oplog Retention

WiredTiger `OplogTruncateMarkers` obey an `oplogMinRetentionHours` configurable setting. When
`oplogMinRetentionHours` is active, the WT `OplogTruncateMarkers` will only truncate the oplog if a
truncate marker (a sequential range of oplog) is not within the minimum time range required to
remain.

## Oplog Hole Truncation

MongoDB maintains an `oplogTruncateAfterPoint` timestamp while in `PRIMARY` and `SECONDARY`
replication modes to track persisted oplog holes. Replication startup recovery uses the
`oplogTruncateAfterPoint` timestamp, if one is found to be set, to truncate all oplog entries after
that point. On clean shutdown, there are no oplog writes and the `oplogTruncateAfterPoint` is
cleared. On unclean shutdown, however, parallel writes can be active and therefore oplog holes can
exist. MongoDB allows secondaries to read their sync source's oplog as soon as there are no
_in-memory_ oplog holes, ensuring data consistency on the secondaries. Primaries, therefore, can
allow oplog entries to be replicated and then lose that data themselves, in an unclean shutdown,
before the replicated oplog entries become persisted. Primaries use the `oplogTruncateAfterPoint`
to continually track oplog holes on disk in order to eliminate them after an unclean shutdown.
Additionally, secondaries apply batches of oplog entries out of order and similarly must use the
`oplogTruncateAfterPoint` to track batch boundaries in order to avoid unknown oplog holes after an
unclean shutdown.

# Error Handling

See
[wtRcToStatus](https://github.com/mongodb/mongo/blob/c799851554dc01493d35b43701416e9c78b3665c/src/mongo/db/storage/wiredtiger/wiredtiger_util.cpp#L178-L183)
where we handle errors from WiredTiger and convert to MongoDB errors.

# Cherry-picked WT log Details

- The WT log is a write ahead log. Before a [transaction commit](https://source.wiredtiger.com/develop/struct_w_t___s_e_s_s_i_o_n.html#a712226eca5ade5bd123026c624468fa2 "WiredTiger Docs") returns to the application, logged writes
  must have their log entry bytes written into WiredTiger's log buffer. Depending on `sync` setting,
  those bytes may or may not be on disk.
- MongoDB only chooses to log writes to a subset of WT's tables (e.g: the oplog).
- MongoDB does not `sync` the log on transaction commit. But rather uses the [log
  flush](https://source.wiredtiger.com/develop/struct_w_t___s_e_s_s_i_o_n.html#a1843292630960309129dcfe00e1a3817 "WiredTiger Docs") API. This optimization is two-fold. Writes that do not require to be
  persisted do not need to wait for durability on disk. Second, this pattern allows for batching
  of writes to go to disk for improved throughput.
- WiredTiger's log is similar to MongoDB's oplog in that multiple writers can concurrently copy
  their bytes representing a log record into WiredTiger's log buffer similar to how multiple
  MongoDB writes can concurrently generate oplog entries.
- MongoDB's optime generator for the oplog is analogous to WT's LSN (log sequence number)
  generator. Both are a small critical section to ensure concurrent writes don't get the same
  timestamp key/memory address to write an oplog entry value/log bytes into.
- While MongoDB's oplog writes are logical (the key is a timestamp), WT's are obviously more
  physical (the key is a memory->disk location). WiredTiger is writing to a memory buffer. Thus before a
  transaction commit can go to the log buffer to "request a slot", it must know how many bytes it's
  going to write. Compare this to a multi-statement transaction replicating as a single applyOps
  versus each statement generating an individual oplog entry for each write that's part of the
  transaction.
- MongoDB testing sometimes uses a [WT debugging
  option](https://github.com/mongodb/mongo/blob/a7bd84dc5ad15694864526612bceb3877672d8a9/src/mongo/db/storage/wiredtiger/wiredtiger_kv_engine.cpp#L601 "Github") that will write "no-op" log entries for other operations performed on a
  transaction. Such as setting a timestamp or writing to a table that is not configured to be
  written to WT's log (e.g: a typical user collection and index).

The most important WT log entry for MongoDB is one that represents an insert into the
oplog.

```
  { "lsn" : [1,57984],
    "hdr_flags" : "compressed",
    "rec_len" : 384,
    "mem_len" : 423,
    "type" : "commit",
    "txnid" : 118,
    "ops": [
		{ "optype": "row_put",
		  "fileid": 14 0xe,
		  "key": "\u00e8^\u00eat@\u00ff\u00ff\u00df\u00c2",
		  "key-hex": "e85eea7440ffffdfc2",
		  "value": "\u009f\u0000\u0000\u0000\u0002op\u0000\u0002\u0000\u0000\u0000i\u0000\u0002ns\u0000\n\u0000\u0000\u0000test.coll\u0000\u0005ui\u0000\u0010\u0000\u0000\u0000\u0004\u0017\u009d\u00b0\u00fc\u00b2,O\u0004\u0084\u00bdY\u00e9%\u001dm\u00ba\u0003o\u00002\u0000\u0000\u0000\u0007_id\u0000^\u00eatA\u00d4\u0098\u00b7\u008bD\u009b\u00b2\u008c\u0002payload\u0000\u000f\u0000\u0000\u0000data and bytes\u0000\u0000\u0011ts\u0000\u0002\u0000\u0000\u0000At\u00ea^\u0012t\u0000\u0001\u0000\u0000\u0000\u0000\u0000\u0000\u0000\twall\u0000\u0085\u001e\u00d6\u00c3r\u0001\u0000\u0000\u0012v\u0000\u0002\u0000\u0000\u0000\u0000\u0000\u0000\u0000\u0000",
		  "value-bson": {
				u'ns': u'test.coll',
				u'o': {u'_id': ObjectId('5eea7441d498b78b449bb28c'), u'payload': u'data and bytes'},
				u'op': u'i',
				u't': 1L,
				u'ts': Timestamp(1592423489, 2),
				u'ui': UUID('179db0fc-b22c-4f04-84bd-59e9251d6dba'),
				u'v': 2L,
				u'wall': datetime.datetime(2020, 6, 17, 19, 51, 29, 157000)}
      }
    ]
  }
```

- `lsn` is a log sequence number. The WiredTiger log files are named with numbers as a
  suffix, e.g: `WiredTigerLog.0000000001`. In this example, the LSN's first value `1` maps to log
  file `0000000001`. The second value `57984` is the byte offset in the file.
- `hdr_flags` stands for header flags. Think HTTP headers. MongoDB configures WiredTiger to use
  snappy compression on its journal entries. Small journal entries (< 128 bytes?) won't be
  compressed.
- `rec_len` is the number of bytes for the record
- `type` is...the type of journal entry. The type will be `commit` for application's committing a
  transaction. Other types are typically for internal WT operations. Examples include `file_sync`,
  `checkpoint` and `system`.
- `txnid` is WT's transaction id associated with the log record.
- `ops` is a list of operations that are part of the transaction. A transaction that inserts two
  documents and removes a third will see three entries. Two `row_put` operations followed by a
  `row_remove`.
- `ops.fileid` refers to the WT table that the operation is performed against. The fileid mapping
  is held in the `WiredTiger.wt` file (a table within itself). This value is faked for WT's
  logging debug mode for tables which MongoDB is not logging.
- `ops.key` and `ops.value` are the binary representations of the inserted document (`value` is omitted
  for removal).
- `ops.key-hex` and `ops.value-bson` are specific to the pretty printing tool used.

[copy-on-write]: https://en.wikipedia.org/wiki/Copy-on-write

# Cache-Pressure Monitor

## Eviction and Application-threads

Wiredtiger tries to keep cache utilization within acceptable ranges. The
process of removing data from the cache (sometimes by writing it to disk) is
called "eviction". Eviction occurs depending on whether the cache's size
exceeds certain "trigger" and "target" limits set by the user:

- `cache < target` : no eviction occurs
- `target <= cache < trigger` : only internal threads will perform eviction.
- `cache >= trigger` : wiredtiger will block application threads from performing
  their work until cache returns to acceptable ranges. This is often called
  "recruitment" because, rather than leave the threads idle, they too are used
  to perform eviction.

## Cache-Pressure and Stalls

Recruiting application threads is associated with periods of unavailability in
the presence of large, long-running transactions.

- Until a transaction finishes, its state is held in-memory and it can not be
  evicted. If enough of these transactions fill the cache, none will be
  evicted and none will make progress (wiredtiger will have recruited them).
- When transactions are committed or aborted, wiredtiger uses that time to
  perform some eviction. This causes unexpected latency increases for many
  operations, and is particularly problematic during shutdown/stepdown as they
  can't complete in a timely fashion.

## Monitoring

To alleviate these problems, the server has mechanisms to detect cache-pressure
and remediate it.

Wiredtiger's implememntation is the `WiredTigerCachePressureMonitor`. This
monitor queries internal wiredtiger stats to determine:

- if the triggers have been reached or exceeded
- how long since we have made any commits/progress
- how much time is being spent by application threads in eviction

If all of the above metrics exceed an allowed limit, a cache-pressure situation
is flagged.

Outside of the storage engine, mongo maintains a thread to perdiodically check
for cache-pressure and act on it, called the
`PeriodicThreadToRollbackUnderCachePressure`. This thread queries wiredtiger
for cache-pressure and, if detected, aborts the oldest transactions until the
cache-pressure goes away.

## Interrupting Eviction

Operations in wiredtiger normally can't be controlled by the mongod process
once it calls a wiredtiger API. This would usually mean we are unable to abort
operations recruited for eviction, in spite of the above monitor.

To solve this, wiredtiger has functionality to periodically query whether a
given application thread should stop evicting, if the
`WT_EVENT_HANDLER.handle_general` returns nonzero for a `WT_EVENT_EVICTION`
event, then wiredtiger will pull that application thread out of eviction early,
either returning normally (for optional kinds of eviction) or erroring (if the
eviction was a mandatory prerequisite for that thread's operation).

This functionality is needed to allow both cache-pressure aborts and
idle-session aborts to roll-back active transactions.

## Detection and Tuning

Cache-pressure detection is not a "one-size-fits-all" solution for end users.
Depending on their workload, how spiky it is, and how many large/long transactions
they use, they may require different configuration to detect/remediate
cache-pressure as it appears for them.

Mongod provides several mechanisms for configuring the cache-pressure
mechanism:

- CachePressureEvictionStallThresholdProportion controls how much our
  application threads' time is allowed to count towards eviction before we
  decide to call it "cache-pressure". Lower values make the monitor more likely
  to decide that we are in cache-pressure.
- CachePressureExponentiallyDecayingMovingAverageAlphaValue controls how
  susceptible the monitor is to mis-detecting cache-pressure when the number of
  read/write tickets varies a lot. Lower values smooth-out the monitor's view
  of tickets.
- CachePressureEvictionStallDetectionWindowSeconds controls how much time the
  monitor will wait while under cache-pressure before allowing rollbacks to
  take place. Lower values make the monitor more responsive.
- CachePressureQueryPeriodMilliseconds controls the frequency that we check for
  cache-pressure. Lower values increase the frequency of checking.
- CachePressureAbortSessionKillLimitPerBatch controls how many transactions
  per-second can be aborted when under cache-pressure. Lower values limit the
  number (i.e. they cause cache-pressure events to either involve fewer aborts
  or take longer to remediate).

# Table of MongoDB <-> WiredTiger <-> Log version numbers

| MongoDB                | WiredTiger | Log |
| ---------------------- | ---------- | --- |
| 3.0.15                 | 2.5.3      | 1   |
| 3.2.20                 | 2.9.2      | 1   |
| 3.4.15                 | 2.9.2      | 1   |
| 3.6.4                  | 3.0.1      | 2   |
| 4.0.16                 | 3.1.1      | 3   |
| 4.2.1                  | 3.2.2      | 3   |
| 4.2.6                  | 3.3.0      | 3   |
| 4.2.6 (blessed by 4.4) | 3.3.0      | 4   |
| 4.4.0                  | 10.0.0     | 5   |
| 5.0.0                  | 10.0.1     | 5   |
| 4.4.11, 5.0.6          | 10.0.2     | 5   |
| 6.0.0                  | 10.0.2     | 5   |
| 6.1.0                  | 11.0.1     | 5   |
| 6.2.0                  | 11.2.0     | 5   |
| 7.0.0                  | 11.2.0     | 5   |
| 7.1.0                  | 11.2.0     | 5   |
| 7.2.0                  | 11.3.0     | 5   |
| 7.3.0                  | 11.3.0     | 5   |
| 8.0.0                  | 12.0.0     | 5   |
