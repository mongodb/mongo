// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/lru_cache.h"
#include "mongo/util/synchronized_value.h"
#include "mongo/util/uuid.h"

#include <cstddef>

namespace mongo::transport::grpc {

/**
 * An LRU cache that is used to track UUIDs provided by clients when creating gRPC streams. It can
 * be used to determine if the server has communicated with a particular client before (e.g. to
 * determine whether its metadata should be logged or not).
 */
class ClientCache {
public:
    enum class AddResult {
        kRefreshed,
        kCreated,
    };

    /**
     * The default maximum number of entries in the cache, which roughly constitutes a few MBs of
     * memory usage when the cache is full (rough calculation: (sizeof(UUID)*2 + a few pointers) *
     * (1 << 16)).
     */
    static constexpr size_t kDefaultCacheSize = 1 << 16;

    explicit ClientCache(size_t maxSize);
    ClientCache() : ClientCache{kDefaultCacheSize} {}

    /**
     * Adds `clientId` to the LRU cache if it doesn't exist, otherwise marks it as the
     * most-recently-used. It may evict the least-recently-used `clientId`.
     */
    AddResult add(const UUID& clientId);

    std::size_t getUniqueClientsSeen() const {
        return _uniqueClientsSeen.load();
    }

private:
    // We only care about whether an ID has been seen, so the cached value is irrelevant.
    struct Data {};

    synchronized_value<LRUCache<UUID, Data, UUID::Hash>> _cache;

    // An APPROXIMATION of unique clients seen over time.
    // As clients fall out of the LRU, reconnects will cause them to be counted again.
    Atomic<std::size_t> _uniqueClientsSeen{0};
};

}  // namespace mongo::transport::grpc
