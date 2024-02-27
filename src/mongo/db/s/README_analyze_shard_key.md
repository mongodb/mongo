# Query Sampling for Shard Key Analyzer

## Introduction

The command `analyzeShardKey` exposes a set of metrics that characterize proposed shard keys
to assist the user in selecting a shard key given a collection's data
and the user's query patterns.
It returns two kinds of metrics called `keyCharacteristics` and `readWriteDistribution`:

-   `keyCharacteristics` consists of metrics about the cardinality, frequency and monotonicity
    of the shard key, calculated based on documents sampled from the collection.
-   `readWriteDistribution` consists of the metrics about query routing patterns
    and the hotness of shard key ranges, calculated based on sampled queries.

The remainder of this document describes how queries are sampled in order to report
`readWriteDistribution` metrics.

## How the Query Analyzer Works

Query sampling is supported on both sharded clusters and standalone replica sets.
It is coordinated by the Query Analyzer which
runs on every mongos of a sharded cluster and on every mongod of a replica set.
For each collection in which the Query Analyzer has been enabled,
the service is responsible for selecting queries to sample,
tracking and maintaining the query sampling rate, and
writing the sampled queries to collections in the `config` database.
The service also runs on shardsvr mongods,
functioning in the same manner as on mongoses.
That is, the Query Analyzer on a shardsvr mongod selects the samples
and updates its sampling rate
for the queries in which the mongod acts as a router.
The `configureQueryAnalyzer` command operates per collection,
setting the Query Analyzer's sampling rate
and enabling or disabling the sampling.
The settings are stored in collection `config.queryAnalyzer`, one document per collection.

### Selecting Queries to Sample

Selection of queries to sample is implemented in
[`QueryAnalysisSampler::tryGenerateSampleId()`](https://github.com/10gen/mongo/blob/a1e2e227762163d8fc405f56708d41ec3d34b550/src/mongo/s/query_analysis_sampler.cpp#L404).
A rate limiter on each sampler is implemented using a
[token bucket algorithm](https://github.com/10gen/mongo/blob/a1e2e227762163d8fc405f56708d41ec3d34b550/src/mongo/s/query_analysis_sampler.cpp#L251),
where a certain number of tokens (may be fractional) are made available each second,
according to the configured sample rate.
Sampling a query requires 1.0 token;
if the bucket has less than 1.0 token, the query is not sampled.

If the query is to be sampled, `tryGenerateSampleId()` generates and returns a unique sample ID.
In a sharded cluster, mongos (or a mongod acting as a router) attaches the sample ID
to the command that it sends out to the shard targeted by the query.
For a query that targets multiple shards, a random shard is selected to receive the sample ID.

### Maintaining Sampling Rate

The selection of sampled queries is based on the `samplesPerSecond` argument
of the `configureQueryAnalyzer` command.
This sampling rate is for the entire collection, not per mongos or mongod.

In a sharded cluster,
the overall `samplesPerSecond` configured via the `configureQueryAnalyzer` command
is divided among mongoses
proportional to the number of queries that each mongos handles,
so a mongos that handles proportionally more queries than other mongoses
also samples proportionally more frequently.
Every query sampler on a mongos maintains an
[exponential moving average](https://github.com/10gen/mongo/blob/a1e2e227762163d8fc405f56708d41ec3d34b550/src/mongo/s/query_analysis_sampler.cpp#L183) of
the number of samples collected over a defined period of time.
This average is [periodically sent](https://github.com/10gen/mongo/blob/a1e2e227762163d8fc405f56708d41ec3d34b550/src/mongo/s/query_analysis_sampler.cpp#L131C48-L131C48) to the config server via the internal command
`_refreshQueryAnalyzerConfiguration`
which in turn responds with the mongos's updated sampling rate.
The per-mongos sampling rate is [computed](https://github.com/10gen/mongo/blob/a1e2e227762163d8fc405f56708d41ec3d34b550/src/mongo/db/s/query_analysis_coordinator.cpp#L252) by the config server using weighted fair queueing,
weighted by the number of queries each mongos routes.

For a standalone replica set, the primary mongod controls the sampling rate on each mongod
in a similar way: computing an exponential moving average of number of samples and updating
its sample rate from the primary.

### Recording and Persisting a Sampled Query

Recording and persisting of sampled queries is designed to minimize the performance impact of
writing sampled queries on the customer's query workload.
Sampled queries are persisted by `QueryAnalysisWriter` by [buffering](https://github.com/10gen/mongo/blob/a1e2e227762163d8fc405f56708d41ec3d34b550/src/mongo/db/s/query_analysis_writer.cpp#L522) them in memory and then
periodically [inserting](https://github.com/10gen/mongo/blob/a1e2e227762163d8fc405f56708d41ec3d34b550/src/mongo/db/s/query_analysis_writer.cpp#L422) them in batches to the collections,
`config.sampledQueries` and `config.sampledQueriesDiff`.

Sampled queries can be fetched via the new aggregation stage called `$listSampledQueries`.
Sampled queries have a TTL of seven days from the insertion time
(configurable by server parameter `sampledQueriesExpirationSeconds`).

### Code references

[**QueryAnalysisSampler class**](https://github.com/10gen/mongo/blob/a1e2e227762163d8fc405f56708d41ec3d34b550/src/mongo/s/query_analysis_sampler.h#L58)

[**QueryAnalysisWriter class**](https://github.com/10gen/mongo/blob/a1e2e227762163d8fc405f56708d41ec3d34b550/src/mongo/db/s/query_analysis_writer.h#L66)

[**QueryAnalysisCoordinator class**](https://github.com/10gen/mongo/blob/a1e2e227762163d8fc405f56708d41ec3d34b550/src/mongo/db/s/query_analysis_coordinator.h#L53)
