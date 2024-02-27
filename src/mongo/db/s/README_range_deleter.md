# Range deletions

The `config.rangeDeletions` collection is a shard-local internal collection containing a document for each range needing to be eventually cleared up; documents in `config.rangeDeletions` are usually referred as "range deletion tasks" or "range deleter documents".

The complete format of range deletion tasks is defined in [range_deletion_tasks.idl](https://github.com/mongodb/mongo/blob/master/src/mongo/db/s/range_deletion_task.idl), with the relevant fields being the following:

-   `collectioUUID`: the UUID of the collection the range belongs to
-   `range`: the `[min, max)` shard key range to be deleted
-   `pending`: boolean flag present if the range is not yet ready for deletion

When a migration starts, a `pending` range deletion task is created both on donor and recipient side and such documents are later modified depending on the migration outcome:

-   Commit: donor range deletion document flagged as ready (remove `pending` flag) AND recipient range deletion document deleted.
-   Abort: donor range deletion document deleted AND recipient range deletion document flagged as ready (remove `pending` flag).

## Range deleter service

The [range deleter service](https://github.com/mongodb/mongo/blob/v7.0/src/mongo/db/s/range_deleter_service.h) is a primary only service living on shards that is driven by the state persisted on `config.rangeDeletions`: for each collection with at least one range deletion task, the service keeps track in memory of the mapping `<collection uuid, [list of ranges containing orphaned documents]>`.

Its main functions are:

-   Scheduling for deletion an orphaned range only when ongoing reads retaining such range have locally drained.
-   Performing the actual deletion of orphaned docs belonging to a range (this happens on a dedicated thread only performing range deletions).
-   Providing a way to know when orphans deletions on specific ranges have completed (callers can get a future to wait for a range to become “orphans free”, such future gets notified only when all range deletion tasks overlapping with the specified range have completed).

### Initialization (service in `INITIALIZING` state)

When a shard node steps up, a thread to asynchronously recover range deletions is spawned (being asynchronous, it does not block step-up); recovery happens in the following steps:

1. Disallow writes on config.rangeDeletions
2. For each document in config.rangeDeletions:
   ---- Register a task on the range deleter service
3. Reallow writes on config.rangeDeletions

### STEADY STATE (service in `UP` state)

The [range deleter service observer](https://github.com/mongodb/mongo/blob/v7.0/src/mongo/db/s/range_deleter_service_op_observer.h) is taking care of keeping in sync the in-memory state with the persistent state by reacting to documents modifications on `config.rangeDeletions` in the following ways:

-   Register a task on the service when a range deletion task document is flagged as ready (insert document without “pending” flag or update existing document to remove such flag)
-   De-register a task from the service when a range deletion task is completed (document deleted)

### SHUTDOWN (service in `DOWN` state)

When a shard primary steps down, the whole service is destroyed:

-   Potentially ongoing orphans cleanup are stopped
-   In-memory state is cleared up

The shutdown of the service is structured in the following way in order not to block step-down:

-   Call for shutdown of the thread performing range deletions
-   On next step-up, join such thread (it's very improbable for the shutdown to have not completed yet following a step-down+up, hence this should be a no-op)
