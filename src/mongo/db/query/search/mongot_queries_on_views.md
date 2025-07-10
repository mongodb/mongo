# `mongot` Queries on Views

##### Definitions

- User pipeline: The aggregation pipeline provided by the user (e.g. `coll.aggregate(<USER PIPELINE>)`).
- View pipeline/definition: TThe pipeline used to define the view, specifying the transformations on the underlying collection (e.g. `db.createView("viewName", "underlyingCollection", <VIEW PIPELINE>)`).
- Request pipeline: The pipeline the server ultimately executes, which may be a modified version of the user pipeline due to optimizations or view logic.
- Search stage: A `$search`, `$vectorSearch`, or `$searchMeta` stage.
- Effective pipeline: The full pipeline applied to a collection to generate the result set, which includes the resolved view pipeline.

## Overview

Search queries on views operate differently from standard view queries. Normally, a query on a view simply prepends the view's pipeline to the user's pipeline. This approach doesn't work for search queries, because a search aggregation must begin with two internal stages: `$_internalSearchMongotRemote` and `$_internalSearchIdLookup`. The requirement for the view pipeline to be at the start of the aggregation is therefore in conflict with the requirements of a search query.

To resolve this, the `$_internalSearchIdLookup` stage applies the view's transformations within its own sub-pipeline. This means the view is applied after the `$_internalSearchMongotRemote` stage but before the rest of the user's pipeline. While this technically violates the rule that a view pipeline must come first, it is permitted because `$_internalSearchMongotRemote` does not modify documents; it only retrieves document IDs from `mongot`.

In summary, `$_internalSearchIdLookup` takes unmodified documents from the `_id` values returned by `$_internalSearchMongotRemote`, applies the view's data transforms, and passes said transformed documents through the rest of the user pipeline [^1].

## Technical Details

### Non-sharded procedure

1. `mongod` receives a query on a view namespace. Since `mongod` has the views' catalog in single node environments, it resolves the view and retrieves the `effectivePipeline` needed to apply the view (see `runAggregateOnView()`).
2. `mongod` recursively calls `_runAggregate()` on the resolved view.
3. In `parsePipelineAndRegisterQueryStats()`, we call `search_helpers::checkAndSetViewOnExpCtx()` before parsing the raw BSON obj vector into a `Pipeline`. As implied, this function sets `view` on `expCtx`, making the aggregation context aware that it is operating on a view.
4. When `parsePipelineAndRegisterQueryStats()` parses the raw BSON obj vector, `createFromBson()` is called on every stage. `createFromBson()` in each of the search stages will first check if there's a view specified on the `DocumentSource`'s spec (there won't be for non-sharded) and calls `search_helpers::getViewFromExpCtx()` to retrieve the view if not. Note that for search queries on _collections_, step 3 will not set the view on `expCtx` (as the aggregation is on a collection, not a view) and therefore `search_helpers::getViewFromExpCtx()` will return `boost::none`. This view is set on the `DocumentSource`'s spec for later use. Additionally, the call to `ResolvedViewAggExState::handleViewHelper()` from `parsePipelineAndRegisterQueryStats()` will skip the step of stitching the view pipeline due to the search stage located within the user pipeline.
5. When the search stage is desugared, the view is passed to `DocumentSourceInternalSearchIdLookup` to be applied. As for `DocumentSourceInternalMongotRemote`, the spec passed to the constructor will later be used in `mongot_cursor::getRemoteCommandRequestForSearchQuery()` when establishing the cursor and sending a request to `mongot`.

### Sharded procedure

1. `mongos` receives the query on the requested namespace forwards it to all shards.
2. The shards do not recognize the namespace because it is an unresolved view. They forward the request to the primary shard, which owns the view catalog and can resolve the view.
3. The primary shard throws a `CommandOnShardedViewNotSupportedOnMongod` exception back to `mongos` with the resolved view info in the exception response.
4. Same as non-sharded step 3, but in `cluster_aggregate.cpp` instead of `run_aggregate.cpp`.
5. Same as non-sharded step 4.
6. When `mongos` serializes the query to perform shard targeting, it serializes the view object directly inside the search stage (see [Serialized Search Stage](#serialized-search-stage)).
7. The targeted shard performs non-sharded step 4 (sharded step 5) again, but this time we expect the view to exist on the spec object because we serialized it from `mongos`. This step demonstrates why we need to store the view on the spec at all as we cannot rely on the `expCtx` to contain the view in sharded environments.
8. Same as non-sharded step 5.

### Stored Source

An important caveat to note about this procedure is the case where a user adds `returnStoredSource: true` to their search query. Assuming that the index is set up appropriately to handle this field, returning `storedSource` means that `mongot` will send back the full document to the server, not just a list of `_id`s to lookup. As this implies, there is no need for `$_internalSearchIdLookup` in this situation as `mongot` will have applied the view on its end. Instead, we will just promote the fields in `$storedSource` to root (`DocumentSourceSearch::desugar()`).

If a user specifies `returnStoredSource: false` or doesn't specify `returnStoredSource` at all in their query, the process above remains the same and `$_internalSearchIdLookup` will be added to the pipeline.

## Examples

### Setup

First, the user creates a view on a collection `underlyingSourceCollection`:

```js
let viewPipeline = [{$addFields: {newField: abc}}];
let addFieldsView = assert.commandWorked(
  db.createView("addFieldsView", "underlyingSourceCollection", viewPipeline),
);
```

Next, the user creates a search index `addFieldsIndex` on that view and runs a search query.

```js
addFieldsView.aggregate([
  {$search: {index: "addFieldsIndex", exists: {path: "_id"}}},
  {$project: {_id: 1}},
]);
```

### Desugared Pipeline

The server internally transforms the user's aggregation into the following pipeline:

```json
[
  {
    "$_internalSearchMongotRemote": {
      "mongotQuery": {
        "$search": {"index": "addFieldsIndex", "exists": {"path": "_id"}}
      },
      "viewName": "addFieldsView" // Only the viewName is accepted by mongot for queries. For index commands (e.g. createSearchIndex), the entire view object is passed (viewName + viewDefinition).
    }
  },
  {
    "$_internalSearchIdLookup": {
      "subPipeline": [
        {
          "$match": "_id"
        },
        {"$addFields": {"newField": "abc"}} // Apply the view after we match by _id.
      ]
    }
  },
  // Rest of the user pipeline.
  {
    "$project": {
      "_id": 1
    }
  }
]
```

### Serialized Search Stage

When `mongos` sends the query to a shard, the view definition is embedded within the $search stage:

```json
{
  "$search": {
    "index": "addFieldsIndex",
    "exists": {"path": "_id"},
    "view": {
      "name": "addFieldsView",
      "effectivePipeline": [{"$addFields": {"newField": "abc"}}]
    }
  }
}
```
