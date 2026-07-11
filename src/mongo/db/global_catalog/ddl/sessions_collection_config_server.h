// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/global_catalog/ddl/sessions_collection_sharded.h"
#include "mongo/util/modules.h"

#include <mutex>

namespace mongo {

class OperationContext;

/**
 * Accesses the sessions collection for config servers.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] SessionsCollectionConfigServer
    : public SessionsCollectionSharded {
public:
    /**
     * Ensures that the sessions collection has been set up for this cluster, sharded, and with the
     * proper indexes.
     *
     * This method may safely be called multiple times and if called concurrently the calls will get
     * serialised using the mutex below.
     *
     * If there are no shards in this cluster, this method will do nothing.
     */
    void setupSessionsCollection(OperationContext* opCtx) override;

private:
    void _shardCollectionIfNeeded(OperationContext* opCtx);
    void _generateIndexesIfNeeded(OperationContext* opCtx);

    // Serialises concurrent calls to setupSessionsCollection so that only one thread performs the
    // sharded operations
    std::mutex _mutex;
};

}  // namespace mongo
