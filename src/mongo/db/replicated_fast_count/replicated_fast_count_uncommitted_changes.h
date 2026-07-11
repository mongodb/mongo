// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/replicated_fast_count/replicated_fast_size_count.h"
#include "mongo/util/uuid.h"

#include <boost/container/flat_map.hpp>

namespace mongo {

class [[MONGO_MOD_PUBLIC]] UncommittedFastCountChange {
public:
    /**
     * Returns an immutable reference to the UncommittedFastCountChange instance associated with
     * this particular OperationContext, returning an empty instance if none exist.
     */
    static const UncommittedFastCountChange& getForRead(OperationContext* opCtx);

    /**
     * Returns a mutable reference to the UncommittedFastCountChange instance associated with
     * this particular OperationContext, creating one if none exist.
     *
     * If an instance is created, a callback will be registered for the RecoveryUnit attached to the
     * OperationContext that will update the changes tracked in _trackedChanges.
     */
    static UncommittedFastCountChange& getForWrite(OperationContext* opCtx);

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
    void record(const NamespaceString& nss, const UUID& uuid, int64_t numDelta, int64_t sizeDelta);

private:
    // Map of collection UUID to uncommitted values for size and count.
    boost::container::flat_map<UUID, CollectionSizeCount> _trackedChanges;
};

}  // namespace mongo

