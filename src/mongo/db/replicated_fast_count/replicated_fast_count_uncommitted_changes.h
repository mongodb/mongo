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
     * Given a collection UUID, returns the current uncommitted value of size and count for that
     * collection. If the collection UUID does not exist, an empty CollectionSizeCount is returned.
     */
    static CollectionSizeCount find(OperationContext* opCtx, UUID uuid);

    /**
     * Records a change in size and count for the given collection UUID. This is a no-op when both
     * deltas are zero or if the provided namespace string is not eligible to be tracked by
     * replicated fast count.
     */
    static void record(OperationContext* opCtx,
                       const NamespaceString& nss,
                       UUID uuid,
                       UncommittedFastCountChange change);

private:
    void _record(const NamespaceString& nss, UUID uuid, UncommittedFastCountChange change);

    CollectionSizeCount _find(UUID uuid) const;

    // Map of collection UUID to its uncommitted size/count delta and the RecordStore that delta
    // must be applied to on commit.
    UncommittedFastCountChangeMap _trackedChanges;
};

}  // namespace mongo

