# Mongot Queries on Views

Search queries on views behave differently than non-search aggregations on views. For the latter, we essentially resolve the view name and append the view pipeline to the beginning of the request pipeline. Not so simple here.

When a request is on a view nss and the user pipeline contains a mongot stage, `idLookup` will apply the view transforms as part of its subpipeline. In this way, the view stages will always be applied directly after `$_internalSearchMongotRemote` and before the remaining stages of the user pipeline. This is to ensure the stages following mongot stage in the user pipeline will receive the modified documents: when `storedSource` is disabled[^1], `idLookup` will retrieve full/unmodified documents during (from the `_id` values returned by mongot), apply the view's data transforms, and pass said transformed documents through the rest of the user pipeline.

For the purposes of posterity, we're going to lay out how this is implemented for both non-sharded and sharded clusters.

## Technical Details

As promised, we're going to run through non-sharded and sharded. Though at the beginning, it's the same for both.

User creates a view:

```
let viewPipeline = [{$match: {$expr: {$gt: [{$subtract: ["$stock", "$num_orders"]}, 300]}}}];
let myMatchView = assert.commandWorked(testDb.createView("myMatchView", "underlyingSourceCollection", viewPipeline));
```

User submits a request on that view:

```
myMatchView.aggregate([{$search: { index: "matchIndex", exists: {path: "_id"}}},{$project: {_id: 1}}]);
```

### Non-sharded

1. mongod receives the query on the requested nss (`myMatchView`) which is the view nss. As this is non-sharded, mongod has the views catalog and resolves the view itself.
2. mongod recursively calls `_runAggregate` on the resolved view.
3. In `parsePipelineAndRegisterQueryStats()`, we call `search_helpers::checkAndAddResolvedNamespaceForSearch()` before parsing the raw bson obj vector into a Pipeline. This is essential because the search helper will add the resolved view to the expression context.
4. When `parsePipelineAndRegisterQueryStats()` parses the raw bson obj vector, createFromBson() is called on every stage. When `DocumentSourceSearch::createFromBson()` is called, it will call a helper, `search_helpers::getViewFromBSONObj()`, that [checks the `expCtx` for the view ns](https://github.com/mongodb/mongo/blob/e2a70df2954e20568c8a1c6fa08c6aa7ffee0d39/src/mongo/db/pipeline/search/search_helper.cpp#L680-L685) that was stored in step 3 to determine if the request is on a view or a normal collection. Since the request is indeed on a view, this helper will return a boost::optional pointer to a custom struct, `MongotQueryViewInfo`, that contains the view information.
5. `DocumentSourceSearch::createFromBson()` will then pass the `MongotQueryViewInfo` obj to the `DocumentSourceSearch` constructor call in order to store the view name + pipeline in a special struct on the `DocumentSourceSearch` object itself.
6. When `$search` is desugared, it will store the view name + pipeline on the `$_internalSearchMongotRemote`[^2] and the view pipeline on `$_internalSearchIdLookup`. This allows us to pass the view name + pipeline in the request to mongot and it allows the server to run the view pipeline as part of `$_internalSearchIdLookup`'s subpieline. The desugared pipeline becomes roughly:

```
{
    $_internalSearchMongotRemote: {
        mongotQuery: {$search: { index: "matchIndex", exists: {path: "_id"}}}
        view: {
            name: "myMatchView",
            effectivePipeline: [{$match: {$expr: {$gt: [{$subtract: ["$stock", "$num_orders"]}, 300]}}}]
        }
        }
},
{
    $_internalSearchIdLookup: {
        subpipeline: [{
                $match: "_id"
            },
            {
                $match: {
                    $expr: {
                        $gt: [{
                            $subtract: [$stock, $num_orders]
                        }, 300]
                    }
                }
            }
        }]
},
{
    $project: {
        _id: 1
    }
}
```

### Sharded

1. Mongos receives the query on the requested nss (`myMatchView`) which is the view nss; mongos subsequently serializes the query with the view nss and sends it to all shards.
2. The shards say "what the heck, idk this nss" and they send it to primary shard which resolves the view using the view catalog it owns.
3. The shards bump this request back to mongos via throwing a `CommandOnShardedViewNotSupportedOnMongod` expection with the resolved view info from the primary shard
4. Mongos does not append the view pipeline to the beginning of the request. Mongos retains the `resolvedView` that is passed back from the shards in `ClusterAggregate::runAggregate()` to be able to persist it through `parsePipelineAndRegisterQueryStats`, as the latter creates the `expCtx`, which will temporarily store the view name and pipeline.
5. Mongos then puts the view name and pipeline on its `expCtx`.
6. When the raw bson obj vector is parsed into a pipeline and `DocumentSourceSearch::createFromBson` is consequently called, it will call a [helper that check the `expCtx` for the view ns](https://github.com/mongodb/mongo/blob/e2a70df2954e20568c8a1c6fa08c6aa7ffee0d39/src/mongo/db/pipeline/search/search_helper.cpp#L680-L685) that was stored in step 5.
7. `DocumentSourceSearch::createFromBson()` will then pass this view to the `DocumentSourceSearch` constructor call in order to store the view name + pipeline in a special struct on the `DocumentSourceSearch` object itself.
8. When mongos serializes the query to perform shard targeting, it serializes the view name and pipeline inside the `$search` stage.

```
{
    $search: {
        index: "matchIndex",
        exists: {path: "_id"},
        view: {
            viewNss: "db.myMatchView",
            effectivePipeline: [{$match: {$expr: {$gt: [{$subtract: ["$stock", "$num_orders"]}, 300]}}}]
            }
        }
}
```

9. The targeted shard receives the BSON request from mongos and [if the `$search` spec includes a view object](https://github.com/mongodb/mongo/blob/e2a70df2954e20568c8a1c6fa08c6aa7ffee0d39/src/mongo/db/pipeline/search/search_helper.cpp#L674-L678), `DocumentSourceSearch::createFromBson()` will pass this view to the `DocumentSourceSearch` constructor call in order to store the view name + pipeline in a special struct on the `DocumentSourceSearch` object itself.
10. When the shard desugars `$search` to <`$_internalSearchMongotRemote`, `$_internalSearchIdLookup`> it will store the view name + pipeline on the former and the view pipeline on the latter. This allows us to pass the view name in the request to mongot and it allows the shard to run the view pipeline as part of `$_internalSearchIdLookup`'s subpieline. The desugared pipeline will look identical to the one presented in `Non-Sharded`

[^1]: For returnStoredSource queries, the documents returned by mongot already include the fields transformed by the view pipeline. As such, mongod doesn't need to apply the view pipeline after `idLookup`.

[^2]: For mongot queries on views, `$_internalSearchMongotRemote` is only required to know the view's nss. However, `DocumentSourceSearchMeta` derives from this class and needs both the view name and the view pipeline. As such, we keep track of the entire view struct.
