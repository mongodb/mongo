/**
 *    Copyright (C) 2023-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <cstddef>

#include "mongo/platform/mutex.h"
#include "mongo/util/lru_cache.h"
#include "mongo/util/synchronized_value.h"
#include "mongo/util/uuid.h"

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

private:
    // We only care about whether an ID has been seen, so the cached value is irrelevant.
    struct Data {};

    synchronized_value<LRUCache<UUID, Data, UUID::Hash>> _cache;
};

}  // namespace mongo::transport::grpc
