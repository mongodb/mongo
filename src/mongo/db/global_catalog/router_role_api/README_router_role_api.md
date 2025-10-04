# Router Role API

## Overview

Any code that needs to route operations to the appropiate shard is said to be operating in the _Router Role_. This contrasts with _Shard Role_ operations, which access data collections directly.

Operations performed in the _Router Role_ must retrieve the routing information for either the target collection or the _DBPrimary_ and dispatch the request to the appropriate shards. If any shard returns an error due to stale routing information on the router node, the operation must refresh the routing data from the config server and retry the entire request.

## CollectionRouter and DBPrimaryRouter

[router_role.h](https://github.com/mongodb/mongo/blob/57f6749350f1c01904c726af505759df3f937424/src/mongo/s/router_role.h) provides the `CollectionRouter` and `DBPrimaryRouter` classes. These should be used to route commands either to the shards that own data for a collection or to the DBPrimary shard of a database respectively.

Here are two usage examples:

```
sharding::router::CollectionRouter router(opCtx->getServiceContext(), nss);

return router.routeWithRoutingContext(
        opCtx,
        "<Comment to identify this process>"sd,
        [&](OperationContext* opCtx, RoutingContext& routingCtx) {
            ...
            // Dispatch a collection request using `routingCtx`
            ...
        }
);
```

```
sharding::router::DBPrimaryRouter router(opCtx->getServiceContext(), nss.dbName());

return router.route(
        opCtx,
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
- [DBPrimaryRouter example](https://github.com/mongodb/mongo/blob/25ddfc96fc2adb2859e91f0401d95b32f3d7af40/src/mongo/db/s/resharding/resharding_manual_cleanup.cpp#L288-L304). In this example, we must target the DBPrimary of the collection’s database to drop the resharding temporary collection. On sharded clusters, it’s important to note that most DDL operations must be directed exclusively to the DBPrimary shard. This is because the DBPrimary is responsible for instantiating a ShardingDDLCoordinator, which coordinates the operation across all shards. To learn more about how DDL operations work in a sharded cluster, go [here](../ddl/README_ddl_operations.md).

These classes handle the following processes internally:

1. Fetch the routing information for the specified collection or DBPrimary shard, and pass it to the lambda function as either a [RoutingContext](../s/query/README_aggregation.md) or a `CachedDatabaseInfo` object.
2. Detect and handle stale routing errors coming from shard responses. If the routing data is outdated, it is automatically refreshed and the operation is retried.
3. Once the operation succeeds, the `RoutingContext` gets validated ([here](../s/query/README_routing_context.md#invariants) you'll find a more clear understanding of what's checked under a `RoutingContext` validation).

When using `CollectionRouter` or `DBPrimaryRouter`, keep the following in mind:

- The lambda function passed to `CollectionRouter::routeWithRoutingContext()` or `DBPrimaryRouter::route()` must use the provided [RoutingContext](../s/query/README_aggregation.md) or `CachedDatabaseInfo` objects to dispatch a shard-versioned command to the shards.
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

multiCollectionRouter.route(
        opCtx,
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
