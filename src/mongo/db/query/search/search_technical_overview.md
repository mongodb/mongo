# Search

This document is a work-in-progress and just provides a high-level overview of the search implementation.

[Atlas Search](https://www.mongodb.com/docs/atlas/atlas-search/) provides integrated full-text search by running queries with the $search and $searchMeta aggregation stages. You can read about the $vectorSearch aggregation stage in [vector_search](https://github.com/mongodb/mongo/blob/master/src/mongo/db/query/vector_search/README.md).

## Lucene

Diving into the mechanics of search requires a brief rundown of [Apache Lucene](https://lucene.apache.org/) because it is the bedrock of MongoDB's search capabilities. MongoDB employees can read more about Lucene and mongot at [go/mongot](http://go/mongot).

Apache Lucene is an open-source text search library, written in Java. Lucene allows users to store data in three primary ways:

- inverted index: maps each term (in a set of documents) to the documents in which the term appears, in which terms are the unique words/phrases and documents are the pieces of content being indexed. Inverted indexes offer great performance for matching search terms with documents.
- storedFields: stores all field values for one document together in a row-stride fashion. In retrieval, all field values are returned at once per document, so that loading the relevant information about a document is very fast. This is very useful for search features that are improved by row-oriented data access, like search highlighting. Search highlighting marks up the search terms and displays them within the best/most relevant sections of a document.
- DocValues: column-oriented fields with a document-to-value mapping built at index time. As it facilitates column based data access, it's faster for aggregating field values for counts and facets.

## `mongot`

`mongot` is a MongoDB-specific process written as a wrapper around Lucene and run on Atlas. Using Lucene, `mongot` indexes MongoDB databases to provide our customers with full text search capabilities.

In the current “coupled” search architecture, one `mongot` runs alongside each `mongod` or `mongos`. Each `mongod`/`mongos` and `mongot` pair are on the same physical box/server and communicate via localhost.

`mongot` replicates the data from its collocated `mongod` node using change streams and builds Lucene indexes on that replicated data. `mongot` is guaranteed to be eventually consistent with mongod. Check out [mongot_cursor](https://github.com/mongodb/mongo/blob/master/src/mongo/db/query/search/mongot_cursor.h) for the core shared code that establishes and executes communication between `mongod` and `mongot`.

## Search Indexes

In order to run search queries, the user has to create a search index. Search index commands similarly use `mongod`/`mongos` server communication protocols to communicate with a remote search index server, but with an Envoy instance that handles forwarding the command requests to Atlas servers and then eventually to the relevant Lucene/`mongot` instances. `mongot` and Envoy instances are co-located with every `mongod` server instance, and Envoy instances are co-located with `mongos` servers as well. The precise structure of the search index architecture will likely evolve in future as improvements are made to that system.

Search indexes can be:

- Only on specified fields ("static")
- All fields (“dynamic”)

`mongot` stores the indexed data exclusively, unless the customer has opted into storing entire documents (more expensive).

There are four search index metadata commands: `createSearchIndexes`, `updateSearchIndex`, `dropSearchIndex` and `listSearchIndexes`. These commands are present on both the `mongod` and `mongos` and are passthrough commands to a remote search index management server. The `mongod`/`mongos` is aware of the address of the remote management server via a startup setParameter `searchIndexManagementHostAndPort`.

The four commands have security authorization action types corresponding with their names. These action types are included in the same built-in roles as the regular index commands, while `updateSearchIndex` parallels collMod.

Note: Indexes can also be managed through the Atlas UI.

## $search and $searchMeta stages

There are two text search stages in the aggregation framework (and $search is not available for find commands). [$search](https://www.mongodb.com/docs/atlas/atlas-search/query-syntax/#-search) returns the results of full-text search, and [$searchMeta](https://www.mongodb.com/docs/atlas/atlas-search/query-syntax/#-searchmeta) returns metadata about search results. When used for an aggregation, either search stage must be the first stage in the pipeline. For example:

```
db.coll.aggregate([
    {$search: {query: "chocolate", path: "flavor"}, returnStoredSource: false},
    {$match: {glutenFree: true}},
    {$project: {"myToken": {$meta: "searchSequenceToken"}, "test": true}}
]);
```

$search and $searchMeta are parsed as [DocumentSourceSearch](https://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/search/document_source_search.h) and [DocumentSourceSearchMeta](https://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/search/document_source_search_meta.h), respectively. When using the classic engine, however, DocumentSourceSearch is [desugared](https://github.com/mongodb/mongo/blob/04f19bb61aba10577658947095020f00ac1403c4/src/mongo/db/pipeline/search/document_source_search.cpp#L118) into a sequence that uses the [$\_internalSearchMongotRemote stage](https://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/search/document_source_internal_search_mongot_remote.h) and, if the `returnStoredSource` option is false, the [$\_internalSearchIdLookup stage](https://github.com/mongodb/mongo/blob/master/src/mongo/db/pipeline/search/document_source_internal_search_id_lookup.h). In SBE, both $search and $searchMeta are lowered directly from the original document sources.

For example, the stage `{$search: {query: “chocolate”, path: “flavor”}, returnStoredSource: false}` will desugar into the two stages: `{$_internalSearchMongotRemote: {query: “chocolate”, path: “flavor”}, returnStoredSource: false}` and `{$_internalSearchIdLookup: {}}`.

### $\_internalSearchMongotRemote

$\_internalSearchMongotRemote is the foundational stage for all search queries, e.g., $search and $searchMeta. This stage opens a cursor on `mongot` ([here](https://github.com/mongodb/mongo/blob/e530c98e7d44878ed8164ee9167c28afc97067a7/src/mongo/db/pipeline/search/document_source_internal_search_mongot_remote.cpp#L269)) and retrieves results one-at-a-time from the cursor ([here](https://github.com/mongodb/mongo/blob/e530c98e7d44878ed8164ee9167c28afc97067a7/src/mongo/db/pipeline/search/document_source_internal_search_mongot_remote.cpp#L163)).

Within this stage, the underlying [TaskExecutorCursor](https://github.com/mongodb/mongo/blob/e530c98e7d44878ed8164ee9167c28afc97067a7/src/mongo/executor/task_executor_cursor.h) acts as a black box to handle dispatching commands to `mongot` only as necessary. The cursor retrieves a batch of results from `mongot`, iterates through that batch per each `getNext` call, then schedules a `getMore` request to `mongot` whenever the previous batch is exhausted.

Each batch returned from mongot includes a batch of BSON documents and metadata about the query results. Each document contains an \_id and a relevancy score. The relevancy score indicates how well the document’s indexed values matched the user query. Metadata is a user-specified group of fields with information about the result set as a whole, mostly including counts of various groups (or facets).

We try to optimize time spent communicating with and waiting on mongot by tuning the `batchSize` option on mongot requests and toggling "prefetch-ing" of GetMore requests. This batchSize-tuning and prefetch-enablement logic is based on an attempt at inferring how many documents need to be requested from mongot (the upper and lower bounds of [`DocsNeededBounds`](https://github.com/mongodb/mongo/blob/03222ee4d38696f293302d0d322b7dac2ccb1e1d/src/mongo/db/pipeline/visitors/docs_needed_bounds.h/#L39)). See [`extractDocsNeededBounds()`](https://github.com/mongodb/mongo/blob/07c5da765d36bfc2bb6bc6d9b101a90cf09e82f7/src/mongo/db/pipeline/visitors/document_source_visitor_docs_needed_bounds.h/#L94) for details on how we traverse the full user pipeline to compute those bounds.

Once the bounds are computed and stored in the document source, we follow a set of heuristics to compute a batchSize for the initial mongot request based on those bounds ([here](https://github.com/mongodb/mongo/blob/03222ee4d38696f293302d0d322b7dac2ccb1e1d/src/mongo/db/query/search/mongot_cursor.cpp/#L110)). The heuristics include applying "oversubscription" logic for non-storedSource queries, to account for the possibility that $\_internalSearchIdLookup may discard some of the documents returned by mongot.

#### Mongot GetMore Stretegy

We customize GetMore-related behaviors of the TaskExecutorCursor (enabling prefetching and tuning the batchSize option) with mongot-specific logic via the `MongotTaskExecutorCursorGetMoreStrategy`.

For example, if we know that we will need all documents from mongot in order to satisfy the query (for example, if the post-$search pipeline has a blocking stage like $sort or $group), then we'll immediately pre-fetch all GetMore requests and will follow a exponential batchSize growth strategy per batch. On the other hand, if the query has an extractable limit N, we attempt to retrieve all N documents in the first batch by tuning the initial batchSize; in that case, we'll never pre-fetch, and if a GetMore is actually needed, we'll tune the batchSize to try to request all still-needed documents in the next batch. See the [`MongotTaskExecutorCursorGetMoreStrategy`](https://github.com/mongodb/mongo/blob/07c5da765d36bfc2bb6bc6d9b101a90cf09e82f7/src/mongo/db/query/search/mongot_cursor_getmore_strategy.h/#L47) for all heuristics and implementation details.

### $\_internalSearchIdLookup

The $\_internalSearchIdLookup stage is responsible for recreating the entire document to give to the rest of the agg pipeline (in the above example, $match and $project) and for checking to make sure the data returned is up to date with the data on `mongod`, since `mongot`’s indexed data is eventually consistent with `mongod`. For example, if `mongot` returned the \_id to a document that had been deleted, $\_internalSearchIdLookup is responsible for catching; it won’t find a document matching that \_id and then filters out that document. The stage will also perform shard filtering, where it ensures there are no duplicates from separate shards, and it will retrieve the most up-to-date field values. However, this stage doesn’t account for documents that had been inserted to the collection but not yet propagated to `mongot` via the $changeStream; that’s why search queries are eventually consistent but don’t guarantee strong consistency.

### Explains

Like normal explain queries, search explain queries can be run with three different verbosities, "queryPlanner" which does not execute the query, and "executionStats" and "allPlansExecution" which do execute the query and output execution stats about the query.

For queries with "queryPlanner" verbosity, we specify "queryPlanner" in our query to mongot, it returns an explain object without a cursor. We directly return this object in our explain output.

For queries with "executionStats" or "allPlansExecution" verbosity levels, we follow the same path as normal search queries to establish cursor(s) on mongot. By including the explain verbosity in our query to mongot, we receive an explain object along with the usual cursor(s) containing documents. These documents are then returned to the subsequent stages of the pipeline, and the execution of the query continues. It's important to note that the merge phase of a sharded query is not executed during an explain (see [SPM-3100](https://jira.mongodb.org/browse/SPM-3100)). If a `getMore` command is issued against the cursor, mongot will return a new explain object which contains updated statistics on its execution of the query. The latest explain object is stored on the [TaskExecutorCursor](https://github.com/mongodb/mongo/blob/a71fa6a39a916983c38c23684cd23ac930ae5616/src/mongo/executor/task_executor_cursor.h#L267) as it handles the `getMore`s. We include the latest explain object from mongot in the explain for [$\_internalSearchMongotRemote, $searchMeta](https://github.com/mongodb/mongo/blob/a71fa6a39a916983c38c23684cd23ac930ae5616/src/mongo/db/pipeline/search/document_source_internal_search_mongot_remote.cpp#L112), and [$vectorSearch](https://github.com/mongodb/mongo/blob/a71fa6a39a916983c38c23684cd23ac930ae5616/src/mongo/db/pipeline/search/document_source_vector_search.cpp#L133-L134) to output the most up to date information.

[comment]: # "TODO SERVER-91594 Remove the following section."

Older versions of mongot do not support returning an explain object alongside the cursor with documents; they only return the explain object. We follow an explain path in [establishCursor()](https://github.com/mongodb/mongo/blob/594f8cff13c7fdd7b71fca48104b2925844be6ad/src/mongo/db/query/search/mongot_cursor.cpp#L224-L227) to handle this case. This special logic will be removed once mongot will always return a cursor with the explain object and the completion of SERVER-91594.

### Didn't Find What You're Looking For?

Visit [the landing page](https://github.com/mongodb/mongo/blob/master/src/mongo/db/query/search/README.md) for all $search/$vectorSearch/$searchMeta related documentation for server contributors.
