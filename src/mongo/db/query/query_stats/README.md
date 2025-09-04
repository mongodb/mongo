# Query Stats

This directory is the home of the infrastructure related to recording runtime query statistics for
the database. It is not to be confused with `src/mongo/db/query/compiler/stats/` which is the home of the
logic for computing and maintaining statistics about a collection or index's data distribution - for
use by the query planner.

The system will collect metrics for each query execution, and the results will be aggregated in a
structure called the [`QueryStatsStore`](#querystatsstore) upon completion of each successful
execution. Metrics will be aggregated according to an abstracted version of the query known as the
query stats key and will be collected on any mongod or mongos process for which they are configured,
including primaries and secondaries.

## QueryStatsStore

At the center of everything here is the [`QueryStatsStore`][query stats store], which is a
partitioned hash table that maps the hash of a [Query Stats Key](#glossary) (also known as the
_Query Stats Store Key_) to some metrics about how often each one occurs.

### Computing the Query Stats Store Key

A query stats store key contains various dimensions that distinctify a specific query. One main
attribute to the query stats store key, is the query shape (`query_shape::Shape`). For example, if
the client does this:

```js
db.example.findOne({x: 24});
db.example.findOne({x: 53});
```

then the `QueryStatsStore` should contain an entry for a single query shape which would record 2
executions and some related statistics (see [`QueryStatsEntry`](query_stats_entry.h) for details).

For more information on query shape, see the [query_shape](../query_shape/README.md) directory.

The query stats store has _more_ dimensions (i.e. more granularity) to group incoming queries than
just the query shape. For example, these queries would all three have the same shape but the first
would have a different query stats store entry from the other two:

```js
db.example.find({x: 55});
db.example.find({x: 55}).batchSize(2);
db.example.find({x: 55}).batchSize(3);
```

There are two distinct query stats store entries here - both the examples which include the batch
size will be treated separately from the example which does not specify a batch size.

### Query Stats vs. Query Shape: Which Options Go Where?

There are two main things to keep in mind when deciding this question:

1. The Query Stats Store Key is generally meant to be the most discriminating way to collect metrics.
   It is always possible for consumers of the metrics to perform their own grouping operation to
   collapse two or more groups back into one, but it is impossible to undo a grouping. That being said,
   we also cannot afford to have infinite entries, so there is a balance. As an example, we do not want
   to track each and every 'comment' separately, since we know of cases where customers may use the
   comment field as a sort of request ID with very high cardinality.

2. The Query Shape is the key used for query settings application. Any two queries with the same
   shape will have the same settings applied. It would then logically follow that any two queries with
   the same query stats store key also have the same query settings applied, since the query shape
   is part of the query stats store key.

3. The Query Shape is generally meant to capture anything semantically important to the query. If an
   option may change the results, it should probably go here. If the option might only impact
   performance or isolation, it should not go here. A couple examples:

- The 'maxTimeMs' is not part of the query shape since it does not matter for the semantics of the
  query - it's purely an operational concern.
- As a trickier example, 'readConcern' was also excluded from the query shape since it only impacts
  isolation guarantees. A shapified version is included in the query stats store key. From a query
  language perspective, it does not really dictate which documents will semantically match the query -
  it's purely a matter of timing. It was deemed more of an operational option/concern. This is a close
  call, because - considering shard filtering and the readConcern level 'available', which does not
  apply shard filtering - a different readConcern level may actually impact which query plan is
  appropriate, and so it probably should play a discriminating role in a plan cache key. A key thought
  experiment here was that a query setting for two identical queries which differ only by readConcern
  should probably apply to both.

- Finally, another borderline example, the 'hint' option is **not** part of the query shape because
  it should only impact performance. The 'hint' is part of the query stats store key. Further, the
  team decided that a query setting should likely impact all queries which differ only by hint (and it
  should override that hint). If an operator sees that a particular query shape should prefer one
  index type, it should logically apply to all queries of that shape. One day we may add the ability
  to override a query setting via a hint, but this is the intended behavior for now.

#### Engineering Considerations

The dimensions considered will depend on the command, but can generally be found in the
[`KeyGenerator`](key_generator.h) interface, which will generate the query stats store keys by which
we accumulate statistics. As one example, you can find the
[`FindKey`](find_key.h) which will include all the things tracked in the
`FindCmdQueryStatsStoreKeyComponents` (including `batchSize` shown in this example).

### Query Stats Store Cache Size

The size of the`QueryStatsStore` can be set by the server parameter
[`internalQueryStatsCacheSize`](#server-parameters), and the partitions will be created based off
that. See [`queryStatsStoreManagerRegisterer`][partition calculation comment] for more details about how
the number of partitions and their size is determined; Each partition is an LRU cache, therefore, if
adding a new entry to the partition makes it go over its size limit, the least recently used entries
will be evicted to drop below the max size. Eviction will be tracked in the new [server status
metrics](#server-status-metrics) for queryStats.

## Metric Collection

At a high level, when a query is run and collection of query stats is enabled, during planning we
call [`registerRequest`][register request] in which the query stats store key will be
generated based on the query's shape and the various other dimensions. The key will always be serialized
and stored on the `opDebug`. For commands that support `getMore`s, it will also be stored on the cursor, so that we can
continue to aggregate the operation's metrics until it is complete.

Once the query execution is fully complete, [`writeQueryStats`][write query stats] will be called and
will either retrieve the entry for the key from the store if it exists and update it, or create a new one and add it to the store.
See more details in the [comments][write query stats comments].

### Data-bearing Node Metrics

Some metrics are only known to data-bearing nodes. When a query is selected for query stats
gathering in a sharded cluster, the router requests that the shards gather those metrics and
include them in cursor responses by setting the `includeQueryStatsMetrics` field to `true` in
requests it makes to the shards. The router then aggregates the metrics received from the shards
into its own query stats store. In executing such a query, the local shard may need to send further
queries to other (foreign) shards. In such cases, the local shard forwards the
`includeQueryStatsMetrics` field to the foreign shard(s). The local shard then aggregates the
metrics it receives into those it includes in its response.

### Rate Limiting

Whether or not query stats will be recorded for a specific query execution depends on a Rate
Limiter, which limits the number of recordings based on the [server parameters](#server-parameters). The goal of the rate limiter
is to minimize impact to overall system performance through restricting traffic. Our rate limiter provides two algorithms:
Window-based policy and sample-based policy. Window-based policy limits the number of recordings per second, whereas sample-based
policy limits the fraction of queries to be recorded. If a query is run but the rate limiter decides not to record it, the query
will still execute as expected but query stats will not be updated in the query stats store. See details [here](rate_limiting.h).

### Explain

Non-aggregate command types take separate paths when the command is run as an explain as opposed to when
they are not run as an explain. We do not collect query stats metrics on the explain-only paths. However, aggregate
explains run through the same path as non-explains, so query stats are collected for aggregate explains.

In the aggregate case, `explain` is not included in the query shape, so an aggregation command that has `explain: true`,
vs. the same command without it will have the same query shape. However we do want to collect separate metrics
for these as they are different, so we include `explain` as a dimension in the query stats store key if present (for agg only).

### Views

Queries on views are always run as an aggregation, since the view is defined as a pipeline. Because of this,
query stats for non-aggregate commands on views would be registered and collected as aggregates without intervention.
There are two considerations here:

#### 1. Registering the request

We want all commands on views to be registered as the original command type rather than as an aggregate.
We do this by making sure to call `registerRequest` before the top-level command path redirects to the aggregate
path, which sets the query stats store key on `CurOp`. This will prevent it from being regenerated as an agg.

However, note that there are special cases even beyond this. When a query is rate-limited in the original `registerRequest`
call, or when it is being run as an explain, we will not set the query stats store key, but we still do not want
the aggregate path to register the request. To handle this case, we set the `disableForSubqueryExecution` flag on the
`OpDebug.QueryStatsInfo` struct to indicate that this request should not be registered for query stats.

#### 2. Collecting the metrics

Regardless of where the query stats store key was generated, the aggregate path will attempt to collect metrics
for any query that has a key populated on `OpDebug`. This is acceptable in many cases, but for commands that must
do post-processing after running the view aggregation pipeline (specifically, the distinct command), this results
in incorrect metrics. These commands must take care to not pass the generated query stats store key to the aggregation
path and instead collect metrics on their own after the aggregation pipeline is complete.

### Change Streams

Query stats also behaves a bit differently for change stream queries. For change stream collections, like normal collections,
we will still collect query stats on creation. However, an important difference is that we will actually treat each `getMore` as its own query,
and collect and update query stats for each one rather than accumulating them on the cursor and recording once execution
completes. We have a flag to determine whether the collection has a change stream, [\_queryStatsWillNeverExhaust][query stats will never exhaust],
and decide based on that whether to take the change stream approach.

## Metric Retrieval

To retrieve the stats gathered in the `QueryStatsStore`, there is a new aggregation stage,
`$queryStats`. This stage must be the first in a pipeline and it must be run against the admin
database. The structure of the command is as follows (note `aggregate: 1` reflecting there is no collection):

```js
db.adminCommand({
  aggregate: 1,
  pipeline: [
    {
      $queryStats: {
        tranformIdentifiers: {
          algorithm: "hmac-sha-256",
          hmacKey: BinData(
            8,
            "87c4082f169d3fef0eef34dc8e23458cbb457c3sf3n2",
          ) /* bindata
                subtype 8 - a new type for sensitive data */,
        },
      },
    },
  ],
});
```

`transformIdentifiers` is optional. If not present, we will generate the regular Query Stats Key. If
present:

- `algorithm` is required and the only currently supported option is "hmac-sha-256".
- `hmacKey` is required
- We will generate the [One-way Tokenized](#glossary) Query Stats Key by applying the "hmac-sha-256"
  to the names of any field, collection, or database. Application Name field is not transformed.

The query stats store will output one document for each query stats key, which is structured in the
following way:

```js
{
    key: {/* Query Stats Key */},
    keyHash: string,
    queryShapeHash: string,
    asOf: ISODate(/* … */),
    metrics: {
        execCount:               0,
        firstSeenTimestamp:      ISODate(/* … */),
        latestSeenTimestamp:     ISODate(/* … */),
        docsReturned:            {sum: 0, max: 0, min: 0, sumOfSquares: 0},
        firstResponseExecMicros: {sum: 0, max: 0, min: 0, sumOfSquares: 0},
        totalExecMicros:         {sum: 0, max: 0, min: 0, sumOfSquares: 0},
        lastExecutionMicros:     0,
    }
}
```

- `key`: Query Stats Key.
- `keyHash`: Hash of the Query Stats Store Key representative value. Corresponds to the `key` field.
- `queryShapeHash`: Hash of the Query Shape representative value. Corresponds to the `key.queryShape` field.
  This is particularly useful for cross-referencing query statistics with Persistent Query Settings.
- `asOf`: UTC time when $queryStats read this entry from the store. This will not return the same
  UTC time for each result. The data structure used for the store is partitioned, and each partition
  will be read at a snapshot individually. You may see up to the number of partitions in unique
  timestamps returned by one $queryStats cursor.
- `metrics`: the metrics collected; these may be flawed due to:
  - Server restarts, which will reset metrics.
  - LRU eviction, which will reset metrics.
  - Rate limiting, which will skew metrics.
- `metrics.execCount`: Number of recorded observations of this query.
- `metrics.firstSeenTimestamp`: UTC time taken at query completion (including getMores) for the
  first recording of this query stats store entry.
- `metrics.lastSeenTimestamp`: UTC time taken at query completion (including getMores) for the
  latest recording of this query stats store entry.
- `metrics.docsReturned`: Various broken down statistics for the number of documents returned by
  observation of this query.
- `metrics.firstResponseExecMicros`: Estimated time spent computing and returning the first batch.
- `metrics.totalExecMicros`: Estimated time spent computing and returning all batches, which is the
  same as the above for single-batch queries, as well as for change streams.
- `metrics.keysExamined`: Various broken down statistics for the number of index keys examined while
  executing this query, including getMores.
- `metrics.docsExamined`: Various broken down statistics for the number of documents examined while
  executing this query, including getMores.
- `metrics.workingTimeMillis`: Various broken down statistics for the estimated time spent executing
  this query, excluding time spent blocked.
- `metrics.hasSortStage`: Aggregate counts of the number of query executions that did and did not
  include a sort stage, respectively.
- `metrics.usedDisk`: Aggregate counts of the number of query executions that did and did not use
  disk, respectively.
- `metrics.fromMultiPlanner`: Aggregate counts of the number of query executions that did and did
  not use the multi-planner, respectively. A query is considered to have used the multi-planner
  if any internal query generated as part of its execution used the multi-planner.
- `metrics.fromPlanCache`: Aggregate counts of the number of query executions that did and did
  not use the plan cache, respectively. A query is considered to have not used the plan cache if
  any internal query generated as part of its execution did not use the plan cache.
- `metrics.bytesRead`: Various broken down statistics for the number of bytes read from disk while
  executing this query, including getMores.
- `metrics.readingTime`: Various broken down statistics for the amount of time spent reading from disk
  while executing this query, including getMores.
- `metrics.lastExecutionMicros`: Estimated time spent processing the latest query (akin to
  "totalExecMicros", not "firstResponseExecMicros").
- `metrics.cpuNanos`: Estimated total CPU time spent by a query operation in nanoseconds. This value
  should always be greater than 0 and will not be returned on platforms other than Linux, since collecting
  cpu time is only supported on Linux.
- `metrics.delinquentAcquisitions`: Numbers of time that an execution ticket acquisition overdue by
  a query operation, including getMores.
- `metrics.totalAcquisitionDelinquencyMillis`: Total time in milliseconds that an execution ticket
  acquisition overdue by a query operation, including getMores.
- `metrics.maxAcquisitionDelinquencyMillis`: Maximum time in milliseconds that an execution ticket
  acquisition overdue by a query operation, including getMores.
- `metrics.numInterruptChecksPerSec`: Number of times checkForInterrupt is called per second by a
  query operation, including getMores.
- `metrics.overdueInterruptApproxMaxMillis`: Maximum time in milliseconds that checkForInterrupt was
  delayed for a sampled query operation, including getMores.

#### Permissions

`$queryStats` is restricted by two privilege actions:

- `queryStatsRead` privilege allows running `$queryStats` without passing the `transformIdentifiers`
  options.
- `queryStatsReadTransformed` allows running `$queryStats` with `transformIdentifiers` set. These
  two privileges are included in the clusterMonitor role in Atlas.

### Server Parameters

- `internalQueryStatsCacheSize`:

  - Max query stats store size, specified as a string like "4MB" or "1%". Defaults to 1% of the
    machine's total memory.
  - Query stats store is a LRU cache structure with partitions, so we may be under the cap due to
    implementation.

- `internalQueryStatsRateLimit`:

  - The rate limit is an integer which imposes a maximum number of recordings per second. Default is
    0 which has the effect of disabling query stats collection. Setting the parameter to -1 means
    there will be no rate limit.

- `internalQueryStatsSampleRate`:

  - The sample rate is a floating-point number between 0.0 and 1.0, representing the fraction of
    queries for which stats will be recorded.
    - `0.0` - No query stats are recorded via sampling (0%).
    - `1.0` - All queries are recorded (100%).
  - When both `internalQueryStatsSampleRate` and `internalQueryStatsRateLimit` are set to non-zero
    values, the sampling-based policy (`internalQueryStatsSampleRate`) takes precedence over the
    window-based rate limiter.
  - The default value is `0`. When set to `0`, sampling-based rate limiting is disabled and query
    stats rate limiting falls back to the window-based policy controlled by
    `internalQueryStatsRateLimit`.

- `logComponentVerbosity.queryStats`:
  - Controls the logging behavior for query stats. See [Logging](#logging) for details.

### Logging

Setting `logComponentVerbosity.queryStats` will do the following for each level:

- Level 0 (default): Nothing will be logged.
- Level 1 or higher: Invocations of $queryStats will be logged if and only if the algorithm is
  "hmac-sha-256". The specification of the $queryStats stage is logged, with any provided hmac key
  redacted.
- Level 2 or higher: Nothing extra, reserved for future use.
- Level 3 or higher: All results of any "hmac-sha-256" $queryStats invocation are logged. Each
  result will be its own entry and there will be one final entry that says "we finished".
- Levels 4 and 5 do nothing extra.

### Server Status Metrics

The following will be added to the `serverStatus.metrics`:

```js
queryStats: {
    numEvicted: NumberLong(0),
    numHmacApplicationErrors: NumberLong(0),
    numQueryStatsStoreWriteErrors: NumberLong(0),
    numRateLimitedRequests: NumberLong(0),
    queryStatsStoreSizeEstimateBytes: NumberLong(0)
}
```

# Glossary

**Query Execution**: This term implies the overall execution of what a client would consider one
query, but which may or may not involve one or more getMore commands to iterate a cursor. For
example, a find command and two getMore commands on the returned cursor is one query execution. An
aggregate command which returns everything in one batch is also one query execution.

**One-way Tokenized Object**: A one-way tokenized object has an HMAC hashing function applied to
particular sensitive elements/pieces of an object. It is "one-way" because it is never meant to be
undone. This allows us to detect when two queries are using the same identifiers, but never to
reveal what those identifiers were.

**Query Shape**: [Query Shape](../query_shape/README.md)

**Query Stats Key**: Also known as the _Query Stats Store Key_, this is the collection of attributes
championed by the query shape which identifies one grouping of metrics. The $queryStats stage will
output one document per query stats key - output in the "key" field.

<!-- Links -->

[query stats store]: https://github.com/mongodb/mongo/blob/3cc7cd2a439e25fff9dd26fb1f94057d837a06f9/src/mongo/db/query/query_stats/query_stats.h#L100-L104
[partition calculation comment]: https://github.com/mongodb/mongo/blob/3cc7cd2a439e25fff9dd26fb1f94057d837a06f9/src/mongo/db/query/query_stats/query_stats.cpp#L173-179
[register request]: https://github.com/mongodb/mongo/blob/3cc7cd2a439e25fff9dd26fb1f94057d837a06f9/src/mongo/db/query/query_stats/query_stats.h#L196-L199
[write query stats]: https://github.com/mongodb/mongo/blob/3cc7cd2a439e25fff9dd26fb1f94057d837a06f9/src/mongo/db/query/query_stats/query_stats.h#L253-L258
[write query stats comments]: https://github.com/mongodb/mongo/blob/3cc7cd2a439e25fff9dd26fb1f94057d837a06f9/src/mongo/db/query/query_stats/query_stats.h#L243-L252
[query stats will never exhaust]: https://github.com/mongodb/mongo/blob/8be794e1983e2b24938489ad2b018b630ea9b563/src/mongo/db/clientcursor.h#L510
