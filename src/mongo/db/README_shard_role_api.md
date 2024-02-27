# Shard Role API

## Shard Role API

Any code that accesses data collections with the intention to read or write is said to be operating
in the _Shard Role_. This contrasts with _Router Role_ operations, which do not access data
collections directly — they only route operations to the appropriate shard.

Shard Role operations are sharding-aware and thus require establishing a consistent view of the _storage engine_, _local catalog_
and _sharding catalog_. The storage engine contains the "data". The local catalog contains
shard-local metadata such as indexes and storage options. The sharding catalog contains the sharding
description (whether the collection is sharded, its shard key pattern, etc.) and the
ownership filter (which shard key ranges are owned by this shard).

Shard Role operations are also responsible for validating routing decisions taken by possibly-stale
upstream routers.

## Acquiring collections

[shard_role.h provides](https://github.com/mongodb/mongo/blob/23c92c3cca727209a68e22d2d9cabe46bac11bb1/src/mongo/db/shard_role.h#L333-L375)
the `acquireCollection*` family of primitives to acquire a consistent view of the catalogs for collections and views. Shard role code is required to use these primitives to access collections/views.

```
CollectionAcquisition acquireCollection(OperationContext* opCtx,
                                        CollectionAcquisitionRequest acquisitionRequest,
                                        LockMode mode);

CollectionAcquisitions acquireCollections(
    OperationContext* opCtx,
    std::vector<CollectionAcquisitionRequest> acquisitionRequests,
    LockMode mode);

CollectionOrViewAcquisition acquireCollectionOrView(
    OperationContext* opCtx, CollectionOrViewAcquisitionRequest acquisitionRequest, LockMode mode);

CollectionOrViewAcquisitions acquireCollectionsOrViews(
    OperationContext* opCtx,
    std::vector<CollectionOrViewAcquisitionRequest> acquisitionRequests,
    LockMode mode);

CollectionAcquisition acquireCollectionMaybeLockFree(
    OperationContext* opCtx, CollectionAcquisitionRequest acquisitionRequest);

CollectionAcquisitions acquireCollectionsMaybeLockFree(
    OperationContext* opCtx, std::vector<CollectionAcquisitionRequest> acquisitionRequests);

CollectionOrViewAcquisition acquireCollectionOrViewMaybeLockFree(
    OperationContext* opCtx, CollectionOrViewAcquisitionRequest acquisitionRequest);

CollectionOrViewAcquisitions acquireCollectionsOrViewsMaybeLockFree(
    OperationContext* opCtx, std::vector<CollectionOrViewAcquisitionRequest> acquisitionRequests);
```

The dimensions of this family of methods are:

-   Collection/View: Whether the caller is okay with the namespace potentially corresponding to a view or not.
-   Locks/MaybeLockFree: The "MaybeLockFree" variant will skip acquiring locks if it is allowed given the opCtx state. It must be only used for read operations. An operation is allowed to skip locks if all the following conditions are met:

    -   (i) it's not part of a multi-document transaction,
    -   (ii) it is not already holding write locks,
    -   (iii) does not already have a non-lock-free storage transaction open.

    The normal variant acquires locks.

-   One or multiple acquisitions: The "plural" variants allow acquiring multiple collections/views in a single call. Acquiring multiple collections in the same acquireCollections call prevents the global lock from getting recursively locked, which would impede yielding.

For each collection/view the caller desires to acquire, `CollectionAcquisitionRequest`/`CollectionOrViewAcquisitionRequest` represents the prerequisites for it, which are:

-   `nsOrUUID`: The NamespaceString or uuid of the desired collection/view.
-   `placementConcern`: The sharding placementConcern, also known as ShardVersion and DatabaseVersion, that the router attached.
-   `operationType`: Whether we are acquiring this collection for reading (`kRead`) or for writing (`kWrite`). `kRead` operations will keep the same orphan filter and range preserver across yields. This way, even if chunk migrations commit, the query plan is guaranteed to keep seeing the documents for the owned ranges at the time the query started.
-   Optionally, `expectedUUID`: for requests where `nsOrUUID` takes the NamespaceString form, this is the UUID we expect the collection to have.

If the prerequisites can be met, then the acquisition will succeed and one or multiple `CollectionAcquisition`/`ViewAcquisition` objects are returned. These objects are the entry point for accessing the catalog information, including:

-   CollectionPtr: The local catalog.
-   CollectionDescription: The sharding catalog.
-   ShardingOwnershipFilter: Used to filter out orphaned documents.

Additionally, these objects hold several resources during their lifetime:

-   For locked acquisitions, the locks.
-   For sharded collections, the _RangePreserver_, which prevents documents that became orphans after having established the collectionAcquisition from being deleted.

As an example:

```
CollectionAcquisition collection =
    acquireCollection(opCtx,
                      CollectionAcquisitionRequest(
                          nss, placementConcern, readConcern, operationType /* kRead/kWrite */));

// Access the local catalog
collection.getCollectionPtr().xxxx();

// Access the sharding catalog
collection.getShardingDescription().isSharded();
collection.getShardingFilter();
```

## TransactionResources

`CollectionAcquisition`/`CollectionOrViewAcquisition` are reference-counted views to a `TransactionResources` object. `TransactionResources` is the holder of the acquisition's resources, which include the global/db/collection locks (in case of a locked acquisition), the local catalog snapshot (collectionPtr), the sharding catalog snapshot (collectionDescription) and ownershipFilter.

Copying a `CollectionAcquisition`/`CollectionOrViewAcquisition` object increases its associated `TransactionResources` reference counter. When it reaches zero, the resources are released.

## Acquisitions and query plans

Query plans are to use `CollectionAcquisitions` as the sole entry point to access the different catalogs (e.g. to access a CollectionPtr, to get the sharding description or the orphan filter). Plans should never store references to the catalogs because they can become invalid after a yield. Upon restore, they will find the `CollectionAcquisition` in a valid state.

## Yielding and restoring

`TransactionResources` can be detached from its current operation context and later attached to a different one -- this is the case for getMore. Acquisitions
associated with a particular `TransactionResources` object must only be used by that operation context.

shard_role.h provides primitives for yielding and restoring. There are two different types of yields: One where the operation will resume on the same operation context (e.g. an update write operation), and the other where they will be restored to a different operation context (e.g. a getMore).

The restore procedure checks that the acquisition prerequisites are still met, namely:

-   That the collection still exists and has not been renamed.
-   That the sharding placement concern can still be met. For `kWrite` acquisitions, this means that the shard version has not changed. This can be relaxed for `kRead` acquisitions: It is allowed that the shard version changes, because the RangePreserver guarantees that all documents corresponding to that placement version are still on the shard.

### Yield and restore to the same operation context

[`yieldTransactionResourcesFromOperationContext`](https://github.com/mongodb/mongo/blob/2e0259b3050e4c27d47e353222395d21bb80b9e4/src/mongo/db/shard_role.h#L442-L453)
yields the resources associated with the acquisition, yielding its locks, and returns a `YieldedTransactionResources`
object holding the yielded resources. After that call,
it is illegal to access any of the associated acquisitions. [`restoreTransactionResourcesToOperationContext`](https://github.com/mongodb/mongo/blob/2e0259b3050e4c27d47e353222395d21bb80b9e4/src/mongo/db/shard_role.h#L455-L456)
takes in a `YieldedTransactionResources` object and restores the resources, reacquiring its locks, checks that the prerequisites expressed on the original `CollectionAcquisitionRequest` can still be met, and reattaches the `TransactionResources` to the current operation context. Now it is legal to access the acquisitions again.

```
// Acquire a collection
CollectionAcquisition collection = acquireCollection(opCtx, CollectionAcquisitionRequest(...));
collection.xxxx();

// Make a plan executor and run it for a bit
auto myPlanExecutor = makeSomePlanExecutor(collection);
myPlanExecutor.getNext();
myPlanExecutor.getNext();

// Yield
auto yieldedTransactionResources = yieldTransactionResourcesFromOperationContext();

// It is illegal to use `collection` here

// Restore
restoreTransactionResourcesToOperationContext(std::move(yieldedTransactionResources));

// Continue executing the plan
myPlanExecutor.getNext();
```

### Yield and restore to a different operation context

Operations that build a plan executor and return a cursor to be consumed over repeated getMore commands do so by stashing its resources to the CursorManager. [`stashTransactionResourcesFromOperationContext`](https://github.com/mongodb/mongo/blob/2e0259b3050e4c27d47e353222395d21bb80b9e4/src/mongo/db/shard_role.h#L512-L516) yields the `TransactionResources` and detaches it from the current operation context. The yielded TransactionResources are stashed to to the CursorManager.

When executing a getMore, the yielded TransactionResources is retrieved from the CursorManager and attached to the new operation context. This is done by constructing the `HandleTransactionResourcesFromCursor` RAII object. Its destructor will re-stash the TransactionResources back to the CursorManager. In case of failure during getMore, `HandleTransactionResourcesFromCursor::dismissRestoredResources()` must be called to dismiss its resources.

As an example, build a PlanExectuor and stash it to the CursorManager:

```
CollectionAcquisition collection = acquireCollection(opCtx1, CollectionAcquisitionRequest(nss, kRead, ...));

// Make a plan executor and run it for a bit
auto myPlanExecutor = makeSomeReadPlanExecutor(opCtx1, collection);
while (...) {
    executor.getNext();
}

ClientCursorPin pinnedCursor = CursorManager::get(opCtx)->registerCursor(...);

// Save the TransactionResources on the CursorManager.
stashTransactionResourcesFromOperationContext(opCtx, pinnedCursor.getCursor());

// [Command ends]
```

And now getMore consumes more documents from the cursor:

```
// --------
// [getMore command]
auto cursorPin = uassertStatusOK(CursorManager::get(opCtx2)->pinCursor(opCtx2, cursorId));

// Restore the stashed TransactionResources to the current opCtx.
HandleTransactionResourcesFromCursor transactionResourcesHandler(opCtx2, cursorPin.getCursor());

// Resume executing the plan
while (...) {
    executor.getNext();
}

// ~HandleTransactionResourcesFromCursor will re-stash the TransactionResources to 'cursorPin'
```
