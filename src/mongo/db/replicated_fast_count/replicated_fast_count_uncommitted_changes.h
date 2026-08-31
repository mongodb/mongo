// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/replicated_fast_count/replicated_fast_size_count.h"
#include "mongo/util/uuid.h"

#include <boost/container/flat_map.hpp>

namespace mongo {

class RecordStore;

/**
 * The uncommitted size/count changes to the associated `RecordStore`.
 *
 * The `RecordStore*` must be stored for correctness. At commit time, the `RecordStore` size/count
 * atomics are updated, but the collection catalog may not contain a newly created collection yet,
 * so the `RecordStore` instance cannot be looked up through the catalog.
 *
 * `recordStore` is a non-owning pointer because the lifetime of the `RecordStore` must exceed the
 * lifetime of `this`. A `RecordStore` instance is only destroyed when its corresponding collection
 * is dropped from the catalog. A collection drop requires an X lock, so it cannot interleave with
 * the operations that commit these changes (insert, update, delete).
 */
struct [[MONGO_MOD_PUBLIC]] UncommittedFastCountChange {
    CollectionSizeCount delta;
    RecordStore* recordStore = nullptr;
};

using UncommittedFastCountChangeMap = boost::container::flat_map<UUID, UncommittedFastCountChange>;

class [[MONGO_MOD_PUBLIC]] UncommittedFastCountChanges {
public:
    /**
     * Returns an immutable reference to the UncommittedFastCountChanges instance associated with
     * this particular OperationContext, returning an empty instance if none exist.
     */
    static const UncommittedFastCountChanges& getForRead(OperationContext* opCtx);

    /**
     * Returns a mutable reference to the UncommittedFastCountChanges instance associated with
     * this particular OperationContext, creating one if none exist.
     *
     * If an instance is created, a callback will be registered for the RecoveryUnit attached to the
     * OperationContext that will update the changes tracked in _trackedChanges.
     */
    static UncommittedFastCountChanges& getForWrite(OperationContext* opCtx);

    /**
     * Given a collection UUID, returns the current uncommitted value of size and count for that
     * collection. If the collection UUID does not exist, an empty CollectionSizeCount is returned.
     */
    CollectionSizeCount find(const UUID& uuid) const;

    /**
     * Records a change in count and size for the given collection UUID. This is a no-op when both
     * deltas are zero or if the provided namespace string is not eligible to be tracked by the
     * replicated fast count collection.
     */
    void record(const NamespaceString& nss, const UUID& uuid, UncommittedFastCountChange change);

private:
    // Map of collection UUID to its uncommitted size/count delta and the RecordStore that delta
    // must be applied to on commit.
    UncommittedFastCountChangeMap _trackedChanges;
};

}  // namespace mongo

