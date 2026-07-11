// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/grpc/client_cache.h"

#include "mongo/platform/compiler.h"

namespace mongo::transport::grpc {

ClientCache::ClientCache(size_t maxSize) : _cache{LRUCache<UUID, Data, UUID::Hash>{maxSize}} {}

ClientCache::AddResult ClientCache::add(const UUID& clientId) {
    auto syncCache = _cache.synchronize();
    if (MONGO_likely(syncCache->find(clientId) != syncCache->end())) {
        return AddResult::kRefreshed;
    }
    _uniqueClientsSeen.fetchAndAddRelaxed(+1);
    (void)syncCache->add(clientId, {});
    return AddResult::kCreated;
}

}  // namespace mongo::transport::grpc
