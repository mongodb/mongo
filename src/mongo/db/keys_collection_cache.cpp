/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/keys_collection_cache.h"

#include "mongo/db/keys_collection_client.h"
#include "mongo/db/keys_collection_document.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/util/str.h"

namespace mongo {

KeysCollectionCache::KeysCollectionCache(std::string purpose, KeysCollectionClient* client)
    : _purpose(std::move(purpose)), _client(client) {}

StatusWith<KeysCollectionDocument> KeysCollectionCache::refresh(OperationContext* opCtx) {
    LogicalTime newerThanThis;

    decltype(_cache)::size_type originalSize = 0;

    {
        stdx::lock_guard<stdx::mutex> lk(_cacheMutex);
        auto iter = _cache.crbegin();
        if (iter != _cache.crend()) {
            newerThanThis = iter->second.getExpiresAt();
        }

        originalSize = _cache.size();
    }

    // Don't allow this to read during initial sync because it will read at the initialDataTimestamp
    // and that could conflict with reconstructing prepared transactions using the
    // initialDataTimestamp as the prepareTimestamp.
    if (repl::ReplicationCoordinator::get(opCtx) &&
        repl::ReplicationCoordinator::get(opCtx)->getMemberState().startup2()) {
        return {ErrorCodes::InitialSyncActive,
                "Cannot refresh keys collection cache during initial sync"};
    }

    auto refreshStatus = _client->getNewKeys(opCtx, _purpose, newerThanThis, true);

    if (!refreshStatus.isOK()) {
        return refreshStatus.getStatus();
    }

    auto& newKeys = refreshStatus.getValue();

    stdx::lock_guard<stdx::mutex> lk(_cacheMutex);
    if (originalSize > _cache.size()) {
        // _cache cleared while we getting the new keys, just return the newest key without
        // touching the _cache so the next refresh will populate it properly.
        // Note: newKeys are sorted.
        if (!newKeys.empty()) {
            return std::move(newKeys.back());
        }
    }

    for (auto&& key : newKeys) {
        _cache.emplace(std::make_pair(key.getExpiresAt(), std::move(key)));
    }

    if (_cache.empty()) {
        return {ErrorCodes::KeyNotFound, "No keys found after refresh"};
    }

    return _cache.crbegin()->second;
}

StatusWith<KeysCollectionDocument> KeysCollectionCache::getKeyById(long long keyId,
                                                                   const LogicalTime& forThisTime) {
    stdx::lock_guard<stdx::mutex> lk(_cacheMutex);

    for (auto iter = _cache.lower_bound(forThisTime); iter != _cache.cend(); ++iter) {
        if (iter->second.getKeyId() == keyId) {
            return iter->second;
        }
    }

    return {ErrorCodes::KeyNotFound,
            str::stream() << "Cache Reader No keys found for " << _purpose
                          << " that is valid for time: "
                          << forThisTime.toString()
                          << " with id: "
                          << keyId};
}

StatusWith<KeysCollectionDocument> KeysCollectionCache::getKey(const LogicalTime& forThisTime) {
    stdx::lock_guard<stdx::mutex> lk(_cacheMutex);

    auto iter = _cache.upper_bound(forThisTime);

    if (iter == _cache.cend()) {
        return {ErrorCodes::KeyNotFound,
                str::stream() << "No key found that is valid for " << forThisTime.toString()};
    }

    return iter->second;
}

void KeysCollectionCache::resetCache() {
    // keys that read with non majority readConcern level can be rolled back.
    if (!_client->supportsMajorityReads()) {
        stdx::lock_guard<stdx::mutex> lk(_cacheMutex);
        _cache.clear();
    }
}

}  // namespace mongo
