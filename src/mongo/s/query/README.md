# Distributed queries

On sharded clusters, the router will route queries to the shard(s) that own the ranges relevant to the query predicate. If the query includes the shard key, then a router will only target the shard(s) that own that shard key (or ranges of shard keys). Conversely, if the query does not include the shard key, then a router will broadcast to all shards that own data for the collection.

Routers use a cache of collection routing tables to determine what shard owns each chunk of the collection. This cache can sometimes be stale (e.g. after a chunk migration commits). In order to ensure that the query was routed correctly, mongos will use the [shard versioning protocol](https://github.com/mongodb/mongo/blob/master/src/mongo/db/s/README_versioning_protocols.md) when forwarding requests to the shard. This ensures that the routing table used for targeting was not stale — if it was, shards will reject the request and inform the router that its routing information is stale.

If more than one shard was targeted, then the results returned by each shard will be merged by the router which will then return the results to the client.

For further reading, the "Practical MongoDB Aggregations" books has a [section](https://www.practical-mongodb-aggregations.com/guides/sharding.html) dedicated to sharding.

## Aggregation pipeline splitting

Routers will target aggregation pipelines to all shards that own relevant data, so that each shard can execute the pipeline in parallel. However, there are certain stages that require aggregating the data from all the shards in one single place. Some examples of this are `$sort`, `$limit`, and `$group`. During query planning, when such a stage is encountered, the pipeline will need to be _split_. The first part or the split pipeline, called _shards part_, will be executed in parallel by all the targeted shards. The second part, called _merger part_, will run on a single node that will gather the results from all the shards that executed the first part and then execute the second part of the pipeline.

Generally, it is desirable that the split point occurs as late as possible in the pipeline. This way, shards execute in parallel as much work as possible.

Stage implementations specify their splitting requirements by implementing the [`DocumentSource::distributedPlanLogic`](https://github.com/mongodb/mongo/blob/d6d5b3e61039d209bba7bd7eb4948830c7f81de6/src/mongo/db/pipeline/document_source.h#L763-L771) method.

## Routing of aggregations that involve multiple collections

Some aggregation stages reference a second (or more) collections. Some examples of this are the following stages: $lookup, $graphLookup, $out, $merge, $unionWith.

Routing of these pipelines generally follows the same approach as for single-collection pipelines — they are targeted according to the _main_ (leftmost) collection. Then, each targeted shard will execute the pipeline and when it comes the time to execute one of the stages that references a secondary collection, will in turn query the shard that owns the relevant data ranges for it — this is, the shard will then behave as a router.

Even when a pipeline does not need to split for correctness reasons, some pipelines that involve multiple collections can be executed more performantly when split and merged on a particular shard. This can minimize network round trips during its execution. This type of split is not necessary for correctness, but it improves performance.

As an example, let's take a look at the following aggregate: `db.sharded.aggregate([{'$lookup': {from: 'unsharded', ...}}])`. This pipeline could be executed by having each shard that owns data for the 'sharded' collection query the shard that owns 'unsharded' for each input document it needs to join. This would be correct, but it is more performant to split the pipeline so that the shard that own the 'unsharded' collection acts as the _merging shard_. This shard will gather the results from the shards that own the 'sharded' collection and then perform local reads to join with 'unsharded'.

This splitting heuristic is also controlled by the [`DocumentSource::distributedPlanLogic`](https://github.com/mongodb/mongo/blob/d6d5b3e61039d209bba7bd7eb4948830c7f81de6/src/mongo/db/pipeline/document_source.h#L763-L771) methods. The choice of the merging shard is controlled by the [stage constraints](https://github.com/mongodb/mongo/blob/d6d5b3e61039d209bba7bd7eb4948830c7f81de6/src/mongo/db/pipeline/stage_constraints.h#L383-L384).

This splitting decision is made by the router during query planning, taking into consideration the placement of the referenced _secondary_ collections. Since the router only has a (possibly stale) cache of the routing information, this decision may sometimes be wrong. And since the shard versioning protocol is only used for the targeting to the _main_ collection, it will not detect the router staleness of the secondary collections placement. This is fine, because this split/merge choice does not affect correctness, but it means that the plan execution would not be as performant as possible. In order to prevent routers from continuing to take splitting decisions based on stale routing information indefinitely, whenever a router has considered the placement of secondary collections during query planning it will attach the 'requestGossipRoutingCache' field to the command-level metadata for the requests directed at the targeted shards (both for the shards and merging parts of the pipeline). On the response, shards will attach their knowledge of the shardVersions for the requested collections on the 'routingCacheGossip' metadata field. This way, routers will learn about new versions of the secondary collections routing table, so that next time they will use an up-to-date routing table.
