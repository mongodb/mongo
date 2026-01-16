# Router Role API

## Overview

Any code that needs to route operations to the appropiate shard is said to be operating in the _Router Role_. This contrasts with _Shard Role_ operations, which access data collections directly.

Operations performed in the _Router Role_ must retrieve the routing information for either the target collection or the _DBPrimary_ and dispatch the request to the appropriate shards. If any shard returns an error due to stale routing information on the router node, the operation must refresh the routing data from the config server and retry the entire request.

## CollectionRouter and DBPrimaryRouter

[router_role.h](https://github.com/mongodb/mongo/blob/57f6749350f1c01904c726af505759df3f937424/src/mongo/s/router_role.h) provides the `CollectionRouter` and `DBPrimaryRouter` classes. These should be used to route commands either to the shards that own data for a collection or to the DBPrimary shard of a database respectively.

Here are two usage examples:

```
sharding::router::CollectionRouter router(opCtx, nss);

return router.routeWithRoutingContext(
        "<Comment to identify this process>"sd,
        [&](OperationContext* opCtx, RoutingContext& routingCtx) {
            ...
            // Dispatch a collection request using `routingCtx`
            ...
        }
);
```

```
sharding::router::DBPrimaryRouter router(opCtx, nss.dbName());

return router.route(
        "<Comment to identify this process>"sd,
        [&](OperationContext* opCtx, const CachedDatabaseInfo& dbInfo) {
            ...
            // Dispatch a DBPrimary request using `dbInfo`
            ...
        }
);
```

You can also find below two real usage examples for each case:

- [CollectionRouter example](https://github.com/mongodb/mongo/blob/66405cdf815cdd2504ea4360f3317657e0dbda92/src/mongo/db/s/rename_collection_coordinator.cpp#L630-L642). In this case we need to create indexes for the `config.system.sessions` collection to on all the shards owning data for that collection.
- [DBPrimaryRouter example](https://github.com/mongodb/mongo/blob/25ddfc96fc2adb2859e91f0401d95b32f3d7af40/src/mongo/db/s/resharding/resharding_manual_cleanup.cpp#L288-L304). In this example, we must target the DBPrimary of the collection’s database to drop the resharding temporary collection. On sharded clusters, it’s important to note that most DDL operations must be directed exclusively to the DBPrimary shard. This is because the DBPrimary is responsible for instantiating a ShardingDDLCoordinator, which coordinates the operation across all shards. To learn more about how DDL operations work in a sharded cluster, go [here](../global_catalog/ddl/README_ddl_operations.md).

These classes handle the following processes internally:

1. Fetch the routing information for the specified collection or DBPrimary shard, and pass it to the lambda function as either a [RoutingContext](./README_routing_context.md) or a `CachedDatabaseInfo` object.
2. Detect and handle stale routing errors coming from shard responses. If the routing data is outdated, it is automatically refreshed and the operation is retried.
3. Once the operation succeeds, the `RoutingContext` gets validated ([here](./README_routing_context.md#invariants) you'll find a more clear understanding of what's checked under a `RoutingContext` validation).

When using `CollectionRouter` or `DBPrimaryRouter`, keep the following in mind:

- The lambda function passed to `CollectionRouter::routeWithRoutingContext()` or `DBPrimaryRouter::route()` must use the provided [RoutingContext](./README_routing_context.md) or `CachedDatabaseInfo` objects to dispatch a shard-versioned command to the shards. The recommended approach is to use the [Scatter-Gather API](#scatter-gather-api).
- Any stale routing error returned by a shard must be thrown so that it can be properly handled by the router logic.
- During a single routing operation, it is crucial to consult only one version of the routing table.

For more details on routing internals, see the [Versioning Protocols](../versioning_protocol/README_versioning_protocols.md) architecture guide.

## MultiCollectionRouter

The `MultiCollectionRouter` extends the functionality of the `CollectionRouter` by enabling routing to multiple collections within a single router role loop. This is particularly useful in scenarios where a code block may encounter stale routing errors from more than one collection.

A common use case is an aggregation pipeline that includes multiple `$lookup` stages. These stages query different foreign collections within the same execution context. If any of these collections has stale routing information, the entire operation must be retried.

```
std::vector<NamespaceString> nssList{nss1,nss2};

sharding::router::MultiCollectionRouter multiCollectionRouter(
    opCtx->getServiceContext(),
    nssList
);

multiCollectionrouter.route(
        "<Comment to identify this process>"sd,
        [&](OperationContext* opCtx,
                const stdx::unordered_map<NamespaceString, CollectionRoutingInfo>& criMap) {
                    ...
                    // Dispatch commands using the `criMap`
                    ...
        }
);
```

You can also find a real usage example for the `MultiCollectionRouter` [here](https://github.com/mongodb/mongo/blob/8ceda1cf09d3b04d3010136777576f8ddd405f94/src/mongo/db/pipeline/initialize_auto_get_helper.h#L99-L124).

## Router Command Distribution and Versioning API

This section describes the utility APIs that support router-side operations in MongoDB's sharded cluster architecture. These utilities work in conjunction with the Router Role API to provide standardized methods for shard versioning, command distribution, and query targeting.

While the Router Role API manages the high-level workflow (routing context lifecycle, retry logic, and validation), these utilities handle the implementation details of targeting shards, attaching version metadata, and dispatching commands.

### Scatter-Gather API

The scatter-gather family of functions provides high-level abstractions for dispatching versioned commands to multiple shards and aggregating their responses.

#### scatterGatherVersionedTargetByRoutingTable

Executes versioned commands against shards determined by query targeting logic. If the query is empty, the command runs on all shards that own chunks for the collection.

```cpp
std::vector<AsyncRequestsSender::Response> scatterGatherVersionedTargetByRoutingTable(
    OperationContext* opCtx,
    RoutingContext& routingCtx,
    const NamespaceString& nss,
    const BSONObj& cmdObj,
    const ReadPreferenceSetting& readPref,
    Shard::RetryPolicy retryPolicy,
    const BSONObj& query,
    const BSONObj& collation
    // ... additional parameters
);
```

**Workflow:**

1. Calls buildVersionedRequestsForTargetedShards() to:
   - Analyze the query against the routing table
   - Determine which shards own matching data
   - Build versioned command objects for each target shard
2. Dispatches commands via `gatherResponses()`
3. Returns aggregated responses

Here is an example of how it works together with the Router Role API:

```cpp
#include "src/mongo/db/router_role/router_role.h"
#include "src/mongo/db/router_role/cluster_commands_helpers.h"  // Contains utility APIs

// Complete router operation using all API layers
StatusWith<BSONObj> executeShardedQuery(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const BSONObj& query) {

    // ROUTER ROLE API: Set up routing workflow
    sharding::router::CollectionRouter router(opCtx, nss);

    return router.routeWithRoutingContext(
        "Complete sharded query example",
        [&](OperationContext* opCtx, RoutingContext& routingCtx) {

            // SCATTER-GATHER API: Automated targeting and dispatch
            auto responses = scatterGatherVersionedTargetByRoutingTable(
                opCtx,
                routingCtx,                    // From Router Role API
                nss,
                BSON("find" << nss.coll()),
                ReadPreferenceSetting(ReadPreference::PrimaryPreferred),
                Shard::RetryPolicy::kIdempotent,
                query,
                BSONObj()
            );

            // Internally, scatter-gather uses:
            // - QUERY TARGETING API to determine shards
            // - SHARD VERSIONING API to attach versions

            // Process results
            return mergeShardResponses(responses);
        }
    );
    // Router Role API handles stale routing errors and retries
}
```

#### scatterGatherVersionedTargetToShards

Executes versioned commands against an explicitly specified set of shards, bypassing query analysis.

```cpp
std::vector<AsyncRequestsSender::Response> scatterGatherVersionedTargetToShards(
    OperationContext* opCtx,
    RoutingContext& routingCtx,
    const DatabaseName& dbName,
    const NamespaceString& nss,
    const BSONObj& cmdObj,
    const ReadPreferenceSetting& readPref,
    Shard::RetryPolicy retryPolicy,
    const std::set<ShardId>& targetShards
);
```

**When to use:**

- When the caller has already determined the target shards
- Operations that need fine-grained control over shard targeting

An example of usage:

```cpp
#include "src/mongo/db/router_role/router_role.h"
#include "src/mongo/db/router_role/cluster_commands_helpers.h"  // Contains utility APIs

// Complete router operation using all API layers
StatusWith<BSONObj> executeShardedQuery(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const BSONObj& query) {

    // ROUTER ROLE API: Set up routing workflow
    sharding::router::CollectionRouter router(opCtx, nss);

    return router.routeWithRoutingContext(
        "Complete targeted sharded query example",
        [&](OperationContext* opCtx, RoutingContext& routingCtx) {

            // Custom targeting logic beyond standard chunk-based routing
            targetedShardsSet = computeShardsToTargetForSpecialCase(routingCtx);

            // SCATTER-GATHER API: Explicitly target computed shard set
            auto response = scatterGatherVersionedTargetToShards(
                opCtx,
                routingCtx,                    // From Router Role API
                DatabaseName::kAdmin,          // Custom database name
                targetedShardsSet,
                BSON("find" << nss.coll()),
                ReadPreferenceSetting(ReadPreference::PrimaryPreferred),
                Shard::RetryPolicy::kIdempotent,
                query,
                BSONObj()
            ).front();

            return response;
        }
    );
    // Router Role API handles stale routing errors and retries
}
```

### When to Use Lower-Level APIs

Most router-side operations should use the high-level scatter-gather functions. Direct use of lower-level APIs like `buildVersionedRequests`/`gatherResponses` is only permitted in exceptional cases:

- Complex aggregation pipelines requiring custom shard targeting logic
- Operations needing fine-grained control over request building
- Special cases where standard targeting doesn't apply (e.g., sharded_agg_helpers.cpp using RemoteCursor API)

### Shard Versioning API

#### Attaching ShardVersion to Commands

All router-side operations that target sharded collections must include versioning metadata to ensure routing consistency and detect stale metadata. Use the standardized appendShardVersion functions to attach version information:

```cpp
// Append shard version to an existing command object
BSONObj appendShardVersion(BSONObj cmdObj, ShardVersion version);

// Append shard version to a BSONObjBuilder
void appendShardVersion(BSONObjBuilder& cmd, ShardVersion version);
```

**Usage example:**

```cpp
BSONObj cmd = BSON("find" << "myCollection");
auto versionedCmd = appendShardVersion(std::move(cmd), routingCtx.getShardVersion(shardId));
```

**Important guidelines:**

- Never manually serialize version information
- Always use appendShardVersion functions to ensure consistent field naming and proper BSON serialization
- Ensure version is attached before sending any command to sharded collections

## Architectural Layers

The Router Role is composed of three complementary API layers:

**ROUTER ROLE API**

- Is composed by the following elements: CollectionRouter, DBPrimaryRouter, MultiCollectionRouter
- Manages routing context lifecycle
- Handles stale routing error detection and retry logic
- Validates routing context after operations

provides RoutingContext to

**SCATTER-GATHER API** (Command Distribution)

- Analyzes queries to determine target shards
- Builds and dispatches versioned commands concurrently
- Aggregates responses from multiple shards

uses

**SHARD VERSIONING** API

- Ensures consistent ShardVersion attachment
- Prevents manual version serialization
- Provides single point of control for versioning
