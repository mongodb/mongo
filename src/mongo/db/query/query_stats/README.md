# Query Stats

This directory is the home of the infrastructure related to recording runtime query statistics for
the database. It is not to be confused with `src/mongo/db/query/stats/` which is the home of the
logic for computing and maintaining statistics about a collection or index's data distribution - for
use by the query planner.

The system will collect metrics for each query execution, and the results will be aggregated in a
structure called the [`QueryStatsStore`](#querystatsstore) upon completion of each successful
execution. Metrics will be aggregated according to an abstracted version of the query known as the
query stats key and will be collected on any mongod or mongos process for which they are configured,
including primaries and secondaries.

## QueryStatsStore

At the center of everything here is the [`QueryStatsStore`](query_stats.h#93-97), which is a
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

The dimensions considered will depend on the command, but can generally be found in the
[`KeyGenerator`](key_generator.h) interface, which will generate the query stats store keys by which
we accumulate statistics. As one example, you can find the
[`FindKey`](find_key.h) which will include all the things tracked in the
`FindCmdQueryStatsStoreKeyComponents` (including `batchSize` shown in this example).

### Query Stats Store Cache Size

The size of the`QueryStatsStore` can be set by the server parameter
[`internalQueryStatsCacheSize`](#server-parameters), and the partitions will be created based off
that. See [`queryStatsStoreManagerRegisterer`](query_stats.cpp#L138-L154) for more details about how
the number of partitions and their size is determined; Each partition is an LRU cache, therefore, if
adding a new entry to the partition makes it go over its size limit, the least recently used entries
will be evicted to drop below the max size. Eviction will be tracked in the new [server status
metrics](#server-status-metrics) for queryStats.

## Metric Collection

At a high level, when a query is run and collection of query stats is enabled, during planning we
call [`registerRequest`](<(query_stats.h#L195-L198)>) in which the query stats store key will be
generated based on the query's shape and the various other dimensions. The key will always be serialized
and stored on the `opDebug`, and also on the cursor in the case that there are `getMore`s, so that we can
continue to aggregate the operation's metrics. Once the query execution is fully complete,
[`writeQueryStats`](query_stats.h#L200-216) will be called and will either retrieve the entry for
the key from the store if it exists and update it, or create a new one and add it to the store. See
more details in the [comments](query_stats.h#L158-L216).

### Rate Limiting

Whether or not query stats will be recorded for a specific query execution depends on a Rate
Limiter, which limits the number of recordings per second based on the server parameter
[internalQueryStatsRateLimit](#server-parameters). The goal of the rate limiter is to minimize
impact to overall system performance through restricting excessive traffic. If a query is run but
the rate limit has been reached, the query will still execute as expected but query stats will not
be updated in the query stats store. Our rate limiter uses the sliding window algorithm; see details
[here](rate_limiting.h#82-87).

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

-   `algorithm` is required and the only currently supported option is "hmac-sha-256".
-   `hmacKey` is required
-   We will generate the [One-way Tokenized](#glossary) Query Stats Key by applying the "hmac-sha-256"
    to the names of any field, collection, or database. Application Name field is not transformed.

The query stats store will output one document for each query stats key, which is structured in the
following way:

```js
{
    key: {/* Query Stats Key */},
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

-   `key`: Query Stats Key.
-   `asOf`: UTC time when $queryStats read this entry from the store. This will not return the same
    UTC time for each result. The data structure used for the store is partitioned, and each partition
    will be read at a snapshot individually. You may see up to the number of partitions in unique
    timestamps returned by one $queryStats cursor.
-   `metrics`: the metrics collected; these may be flawed due to:
    -   Server restarts, which will reset metrics.
    -   LRU eviction, which will reset metrics.
    -   Rate limiting, which will skew metrics.
-   `metrics.execCount`: Number of recorded observations of this query.
-   `metrics.firstSeenTimestamp`: UTC time taken at query completion (including getMores) for the
    first recording of this query stats store entry.
-   `metrics.lastSeenTimestamp`: UTC time taken at query completion (including getMores) for the
    latest recording of this query stats store entry.
-   `metrics.docsReturned`: Various broken down metrics for the number of documents returned by
    observation of this query.
-   `metrics.firstResponseExecMicros`: Estimated time spent computing and returning the first batch.
-   `metrics.totalExecMicros`: Estimated time spent computing and returning all batches, which is the
    same as the above for single-batch queries.
-   `metrics.lastExecutionMicros`: Estimated time spent processing the latest query (akin to
    "totalExecMicros", not "firstResponseExecMicros").

#### Permissions

`$queryStats` is restricted by two privilege actions:

-   `queryStatsRead` privilege allows running `$queryStats` without passing the `transformIdentifiers`
    options.
-   `queryStatsReadTransformed` allows running `$queryStats` with `transformIdentifiers` set. These
    two privileges are included in the clusterMonitor role in Atlas.

### Server Parameters

-   `internalQueryStatsCacheSize`:

    -   Max query stats store size, specified as a string like "4MB" or "1%". Defaults to 1% of the
        machine's total memory.
    -   Query stats store is a LRU cache structure with partitions, so we may be under the cap due to
        implementation.

-   `internalQueryStatsRateLimit`:

    -   The rate limit is an integer which imposes a maximum number of recordings per second. Default is
        0 which has the effect of disabling query stats collection. Setting the parameter to -1 means
        there will be no rate limit.

-   `logComponentVerbosity.queryStats`:
    -   Controls the logging behavior for query stats. See [Logging](#logging) for details.

### Logging

Setting `logComponentVerbosity.queryStats` will do the following for each level:

-   Level 0 (default): Nothing will be logged.
-   Level 1 or higher: Invocations of $queryStats will be logged if and only if the algorithm is
    "hmac-sha-256". The specification of the $queryStats stage is logged, with any provided hmac key
    redacted.
-   Level 2 or higher: Nothing extra, reserved for future use.
-   Level 3 or higher: All results of any "hmac-sha-256" $queryStats invocation are logged. Each
    result will be its own entry and there will be one final entry that says "we finished".
-   Levels 4 and 5 do nothing extra.

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
