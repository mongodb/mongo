# The TTLMonitor

The TTLMonitor runs as a background job on each mongod. On a mongod primary, the TTLMonitor is
responsible for removing documents expired on [TTL
Indexes](https://www.mongodb.com/docs/manual/core/index-ttl/) across the mongod instance. It
continuously runs in a loop that sleeps for
['ttlMonitorSleepSecs'](https://github.com/mongodb/mongo/blob/d88a892d5b18035bd0f5393a42690e705c2007d7/src/mongo/db/ttl.idl#L39)
and then performs a TTL Pass to remove all expired documents.

The TTLMonitor exhibits different behavior pending on whether batched deletes are enabled. When
enabled (the default), the TTLMonitor batches TTL deletions and also removes expired documents more
fairly among TTL indexes. When disabled, the TTLMonitor falls back to legacy, doc-by-doc deletions
and deletes all expired documents from a single TTL index before moving to the next one. The legacy
behavior can lead to the TTLMonitor getting "stuck" deleting large ranges of documents on a single
TTL index, starving other indexes of deletes at regular intervals.

## Fair TTL Deletion

If
['ttlMonitorBatchDeletes'](https://github.com/mongodb/mongo/blob/d88a892d5b18035bd0f5393a42690e705c2007d7/src/mongo/db/ttl.idl#L48)
is specified, the TTLMonitor will batch deletes and provides fair TTL deletion as follows:

- The TTL pass consists of one or more sub-passes.
- Each sub-pass refreshes its view of TTL indexes in the system. It removes documents on each TTL
  index in a round-robin fashion until there are no more expired documents or
  ['ttlMonitorSubPassTargetSecs'](https://github.com/mongodb/mongo/blob/d88a892d5b18035bd0f5393a42690e705c2007d7/src/mongo/db/ttl.idl#L58)
  is reached.
  - The delete on each TTL index removes up to
    ['ttlIndexDeleteTargetDocs'](https://github.com/mongodb/mongo/blob/d88a892d5b18035bd0f5393a42690e705c2007d7/src/mongo/db/ttl.idl#L84)
    or runs up to
    ['ttlIndexDeleteTargetTimeMS'](https://github.com/mongodb/mongo/blob/d88a892d5b18035bd0f5393a42690e705c2007d7/src/mongo/db/ttl.idl#L72),
    whichever target is met first. The same TTL index can be queued up to be revisited in the same
    sub-pass if there are outstanding deletions.
  - A TTL index is not visited any longer in a sub-pass once all documents are deleted.
- If there are outstanding deletions by the end of the sub-pass for any TTL index, a new sub-pass
  starts immediately within the same pass.

_Code spelunking starting points:_

- [_The TTLMonitor
  Class_](https://github.com/mongodb/mongo/blob/d88a892d5b18035bd0f5393a42690e705c2007d7/src/mongo/db/ttl.h)
- [_The TTLCollectionCache
  Class_](https://github.com/mongodb/mongo/blob/d88a892d5b18035bd0f5393a42690e705c2007d7/src/mongo/db/ttl_collection_cache.h)
- [_ttl.idl_](https://github.com/mongodb/mongo/blob/d88a892d5b18035bd0f5393a42690e705c2007d7/src/mongo/db/ttl.idl)

# Timeseries Collections

The TTL monitor will only delete data from a time-series bucket collection when a bucket's minimum
time, \_id, is past the expiration plus the bucket maximum time span (default 1 hour). This
procedure avoids deleting buckets with data that is not older than the expiration time.
