# Operation Resource Consumption Metrics

MongoDB supports collecting per-operation resource consumption metrics. These metrics reflect the
impact operations have on the server. They may be aggregated per-database and queried by an
aggregation pipeline stage `$operationMetrics`.

Per-operation metrics collection may be enabled with the
`profileOperationResourceConsumptionMetrics` server parameter (default off). When the parameter is
enabled, operations collect resource consumption metrics and report them in the slow query logs. If
profiling is enabled, these metrics are also profiled.

Per-database aggregation may be enabled with the `aggregateOperationResourceConsumptionMetrics`
server parameter (default off). When this parameter is enabled, in addition to the behavior
described by the profiling server parameter, operations will accumulate metrics to global in-memory
per-database counters upon completion. Aggregated metrics may be queried by using the
`$operationMetrics` aggregation pipeline stage. This stage returns an iterable, copied snapshot of
all metrics, where each document reports metrics for a single database.

Metrics are not cleared for dropped databases, which introduces the potential to slowly leak memory
over time. Metrics may be cleared globally by supplying the `clearMetrics: true` flag to the
pipeline stage or restarting the process.

## Limitations

Metrics are not collected for all operations. The following limitations apply:

- Only operations from user connections collect metrics. For example, internal connections from
  other replica set members do not collect metrics.
- Metrics are only collected for a specific set of commands. Those commands override the function
  `Command::collectsResourceConsumptionMetrics()`.
- Metrics for write operations are only collected on primary nodes.
  - This includes TTL index deletions.
- All attempted write operations collect metrics. This includes writes that fail or retry internally
  due to write conflicts.
- Read operations are attributed to the replication state of a node. Read metrics are broken down
  into whether they occurred in the primary or secondary replication states.
- Index builds collect metrics. Because index builds survive replication state transitions, they
  only record aggregated metrics if the node is currently primary when the index build completes.
- Metrics are not collected on `mongos` and are not supported or tested in sharded environments.
- Storage engines other than WiredTiger do not implement metrics collection.
- Metrics are not adjusted after replication rollback.

## Document and Index Entry Units

In addition to reporting the number of bytes read to and written from the storage engine, MongoDB
reports certain derived metrics: units read and units written.

Document units and index entry units are metric calculations that attempt to account for the
overhead of performing storage engine work by overstating operations on smaller documents and index
entries. For each observed datum, a document or index entry, a unit is calculated as the following:

```
units = ceil (datum bytes / unit size in bytes)
```

This has the tendency to overstate small datums when the unit size is large. These unit sizes are
tunable with the server parameters `documentUnitSizeBytes` and `indexEntryUnitSizeBytes`.

## Total Write Units

For writes, the code also calculates a special combined document and index unit. The code attempts
to associate index writes with an associated document write, and takes those bytes collectively to
calculate units. For each set of bytes written, a unit is calculated as the following:

```
units = ceil (set bytes / unit size in bytes)
```

To associate index writes with document writes, the algorithm is the following:
Within a storage transaction, if a document write precedes as-yet-unassigned index writes, assign
such index bytes with the preceding document bytes, up until the next document write.
If a document write follows as-yet-unassigned index writes, assign such index bytes with the
following document bytes.

The `totalUnitWriteSizeBytes` server parameter affects the unit calculation size for the above
calculation.

## CPU Time

Operations that collect metrics will also collect the amount of active CPU time spent on the command
thread. This is reported as `cpuNanos` and is provided by the `OperationCPUTimer`.

The CPU time metric is only supported on certain flavors of Linux. It is implemented using
`clock_gettime` and `CLOCK_THREAD_CPUTIME_ID`, which has limitations on certain systems. See the
[man page for clock_gettime()](https://linux.die.net/man/3/clock_gettime).

## Example output

The $operationMetrics stage behaves like any other pipeline cursor, and will have the following
schema, per returned document:

```
{
  db: "<dbname>",
  // Metrics recorded while the node was PRIMARY. Summed with secondaryMetrics metrics gives total
  // metrics in all replication states.
  primaryMetrics: {
    // The number of document bytes read from the storage engine
    docBytesRead: 0,
    // The number of document units read from the storage engine
    docUnitsRead: 0,
    // The number of index entry bytes read from the storage engine
    idxEntryBytesRead: 0,
    // The number of index entry units read from the storage engine
    idxEntryUnitsRead: 0,
    // The number of document units returned by query operations
    docUnitsReturned: 0,
    // These fields are ALWAYS ZERO and only present for backwards compatibility:
    cursorSeeks: 0,
    keysSorted: 0,
    sorterSpills: 0,
  },
  // Metrics recorded while the node was SECONDARY
  secondaryMetrics: {
    docBytesRead: 0,
    docUnitsRead: 0,
    idxEntryBytesRead: 0,
    idxEntryUnitsRead: 0,
    docUnitsReturned: 0,
    // These fields are ALWAYS ZERO and only present for backwards compatibility:
    cursorSeeks: 0,
    keysSorted: 0,
    sorterSpills: 0,
  },
  // The amount of active CPU time used by all operations
  cpuNanos: 0,
  // The number of document bytes attempted to be written to or deleted from the storage engine
  docBytesWritten: 0,
  // The number of document units attempted to be written to or deleted from the storage engine
  docUnitsWritten: 0,
  // The number of index entry bytes attempted to be written to or deleted from the storage engine
  idxEntryBytesWritten: 0,
  // The number of index entry units attempted to be written to or deleted from the storage engine
  idxEntryUnitsWritten: 0,
  // The total number of document plus associated index entry units attempted to be written to
  // or deleted from the storage engine
  totalUnitsWritten: 0
}
```
