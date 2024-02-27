# Vector Search

## Introduction

[Atlas Search](https://www.mongodb.com/docs/atlas/atlas-search/) provides integrated full-text search for running syntactic search queries. However, this does not cover semantic searches, e.g. searching "cat" in hopes of matching "kitten". The vector search feature allows users to run semantic search queries directly in the database. Users may define vector indexes that are maintained externally on a `mongot` process backed by [Apache Lucene](https://lucene.apache.org/). Queries made via the aggregation framework specify a query vector and path to search over, which `mongod` funnels through to `mongot`.

This document covers the `mongod` side of the vector search implementation.

## Overview

Vector search is implemented as an aggregation stage that behaves similarly to [`$search`](https://github.com/mongodb/mongo/blob/master/src/mongo/db/query/search/README.md). The `$vectorSearch` stage must be the first stage in the pipeline, always run on `mongod`. Users specify the query vector and path to search over as well as several `mongot`-specific knobs. `$vectorSearch` fetches results from `mongot` via a cursor-based protocol that parallels (and reuses code from) `$search`.

## Details

### Aggregation Stage

#### Parameters

[`$vectorSearch`](https://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/search/document_source_vector_search.h) takes several [parameters](https://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/search/document_source_vector_search.idl) that are passed on to `mongot`. These include:

| Parameter     | Description                                                 |
| ------------- | ----------------------------------------------------------- |
| queryVector   | vector to query                                             |
| path          | field to search over                                        |
| numCandidates | number of candidates to consider when performing the search |
| limit         | maximum number of documents to return                       |
| index         | index to use for the search                                 |
| filter        | optional pre-filter to apply before searching               |

Validation for most of these fields occurs on `mongot`, with the exception of `filter`. `mongot` does not yet support complex MQL semantics, so the `filter` is limited to simple comparisons (e.g. `$eq`, `$lt`, `$gte`) on basic field types. This is validated on `mongod` with a [custom `MatchExpressionVisitor`](https://github.com/mongodb/mongo/blob/master/src/mongo/db/query/vector_search/filter_validator.cpp).

Additionally, `limit` may be used by `mongod` to ensure correct results in sharded clusters (described below). All other parameters are passed through to `mongot` for algorithm-specific behavior.

#### getMore

The `$vectorSearch` stage supports sending `getMore` requests to `mongot` when a batch is exhausted, but this is not expected to happen often. Because `mongot` receives the maximum number of documents requested via the initial `limit` parameter, it is able to generate all results on the first call. The only situation in which a `getMore` is required is when the result set is large enough to exceed the 16MB response size limit.

### idLookup

An `$_internalSearchIdLookup` stage is [inserted into the pipeline](https://github.com/mongodb/mongo/blob/636d0c1ce26d905cc508a73ada598950e16860b5/src/mongo/db/pipeline/search/document_source_vector_search.cpp#L204) directly after the `$vectorSearch` stage (always on `mongod`) so that full documents can be returned to the user, as vector indexes do not support any kind of stored source functionality.

Note that there are no mitigations in place to handle idLookup reducing the size of the result set when it filters out orphans. The `limit` parameter passed to `$vectorSearch` is understood to be a maximum, so we may generate that number of results and then subsequently drop orphans, ending with fewer than `limit` documents. This differs from `$search`, where we would request more documents from `mongot` to make up for the orphans.

### Metadata

Results are returned in descending score order from `mongot`. A metadata field with this value, `$vectorSearchScore`, is allowed to be projected by the user.

### Sharding

In a sharded environment, results are merged and [sorted in descending order](https://github.com/mongodb/mongo/blob/636d0c1ce26d905cc508a73ada598950e16860b5/src/mongo/db/pipeline/search/document_source_vector_search.h#L62) on the `$vectorSearchScore` metadata field. Additionally, the `limit` parameter specified in `$vectorSearch` is applied after merging by [inserting an additional `$limit` stage]() into the merging pipeline.

If a user-specified `$limit` exists in the pipeline following `$vectorSearch` that is smaller than the `$vectorSearch` limit value, this is pushed down to the shards as well, although it is not sent to `mongot`.

### Index Management

Vector indexes are managed through the existing [search index management commands](https://github.com/mongodb/mongo/blob/master/src/mongo/db/query/search/README.md#search-index-commands), due to the fact that they are stored in the same way as search indexes on `mongot`.

### Testing

The `vectorSearch` command is supported by [`mongotmock`](https://github.com/mongodb/mongo/blob/636d0c1ce26d905cc508a73ada598950e16860b5/src/mongo/db/query/search/mongotmock/mongotmock_commands.cpp#L194) for testing.
