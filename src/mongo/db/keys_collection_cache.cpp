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

#include "mongo/db/keys_collection_cache.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/keys_collection_client.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/util/str.h"

#include <iterator>
#include <mutex>
#include <utility>

#include <boost/move/utility_core.hpp>

namespace mongo {

KeysCollectionCache::KeysCollectionCache(std::string purpose, KeysCollectionClient* client)
    : _purpose(std::move(purpose)), _client(client) {}

StatusWith<KeysCollectionDocument> KeysCollectionCache::refresh(OperationContext* opCtx) {
    // Don't allow this to read during initial sync because it will read at the initialDataTimestamp
    // and that could conflict with reconstructing prepared transactions using the
    // initialDataTimestamp as the prepareTimestamp.
    if (repl::ReplicationCoordinator::get(opCtx) &&
        repl::ReplicationCoordinator::get(opCtx)->getMemberState().startup2()) {
        return {ErrorCodes::InitialSyncActive,
                "Cannot refresh keys collection cache during initial sync"};
    }

    // Don't allow this to read during rollback as the storage engine requires exclusive access.
    if (repl::ReplicationCoordinator::get(opCtx) &&
        repl::ReplicationCoordinator::get(opCtx)->getMemberState().rollback()) {
        return {ErrorCodes::InterruptedDueToReplStateChange,
                "Cannot refresh keys collection cache during rollback"};
    }

    auto refreshStatus = _refreshExternalKeys(opCtx);

    if (!refreshStatus.isOK()) {
        return refreshStatus;
    }

    return _refreshInternalKeys(opCtx);
}

StatusWith<KeysCollectionDocument> KeysCollectionCache::_refreshInternalKeys(
    OperationContext* opCtx) {
    LogicalTime newerThanThis;
    decltype(_internalKeysCache)::size_type originalSize = 0;

    {
        stdx::lock_guard<stdx::mutex> lk(_cacheMutex);
        auto iter = _internalKeysCache.crbegin();
        if (iter != _internalKeysCache.crend()) {
            newerThanThis = iter->second.getExpiresAt();
        }

        originalSize = _internalKeysCache.size();
    }

    auto refreshStatus =
        _client->getNewInternalKeys(opCtx, _purpose, newerThanThis, true /* tryUseMajority */);

    if (!refreshStatus.isOK()) {
        return refreshStatus.getStatus();
    }

    auto& newKeys = refreshStatus.getValue();

    stdx::lock_guard<stdx::mutex> lk(_cacheMutex);
    if (originalSize > _internalKeysCache.size()) {
        // _internalKeysCache cleared while we were getting the new keys, just return the newest key
        // without touching the _internalKeysCache so the next refresh will populate it properly.
        // Note: newKeys are sorted.
        if (!newKeys.empty()) {
            return std::move(newKeys.back());
        }
    }

    for (auto&& key : newKeys) {
        _internalKeysCache.emplace(std::make_pair(key.getExpiresAt(), std::move(key)));
    }

    if (_internalKeysCache.empty()) {
        return {ErrorCodes::KeyNotFound, "No keys found after refresh"};
    }

    return _internalKeysCache.crbegin()->second;
}

Status KeysCollectionCache::_refreshExternalKeys(OperationContext* opCtx) {
    decltype(_externalKeysCache)::size_type originalSize = 0;

    {
        stdx::lock_guard<stdx::mutex> lk(_cacheMutex);
        originalSize = _externalKeysCache.size();
    }

    auto refreshStatus = _client->getAllExternalKeys(opCtx, _purpose);

    if (!refreshStatus.isOK()) {
        return refreshStatus.getStatus();
    }

    auto& newKeys = refreshStatus.getValue();

    std::multimap<long long, ExternalKeysCollectionDocument> newExternalKeysCache;
    for (auto&& key : newKeys) {
        newExternalKeysCache.emplace(key.getKeyId(), std::move(key));
    }

    stdx::lock_guard<stdx::mutex> lk(_cacheMutex);
    if (originalSize > _externalKeysCache.size()) {
        // _externalKeysCache cleared while we were getting the new keys, just return so the next
        // refresh will populate it properly.
        return Status::OK();
    }

    // Replace the cached keys with the newly loaded ones. Note because all external keys are loaded
    // when refreshing them, this will remove keys that have been deleted from the collection.
    std::swap(_externalKeysCache, newExternalKeysCache);

    return Status::OK();
}

StatusWith<KeysCollectionDocument> KeysCollectionCache::getInternalKeyById(
    long long keyId, const LogicalTime& forThisTime) {
    stdx::lock_guard<stdx::mutex> lk(_cacheMutex);

    for (auto iter = _internalKeysCache.lower_bound(forThisTime); iter != _internalKeysCache.cend();
         ++iter) {
        if (iter->second.getKeyId() == keyId) {
            return iter->second;
        }
    }

    return {ErrorCodes::KeyNotFound,
            str::stream() << "Cache Reader No internal keys found for " << _purpose
                          << " that is valid for time: " << forThisTime.toString()
                          << " with id: " << keyId};
}

StatusWith<std::vector<ExternalKeysCollectionDocument>> KeysCollectionCache::getExternalKeysById(
    long long keyId, const LogicalTime& forThisTime) {
    stdx::lock_guard<stdx::mutex> lk(_cacheMutex);
    std::vector<ExternalKeysCollectionDocument> keys;

    if (_externalKeysCache.empty()) {
        return {ErrorCodes::KeyNotFound,
                str::stream() << "Cache Reader No external keys found for " << _purpose
                              << " with id: " << keyId};
    }

    auto keysRange = _externalKeysCache.equal_range(keyId);
    for (auto keyIter = keysRange.first; keyIter != keysRange.second; keyIter++) {
        auto key = keyIter->second;
        if (key.getExpiresAt() > forThisTime) {
            keys.push_back(key);
        }
    }

    if (keys.empty()) {
        return {ErrorCodes::KeyNotFound,
                str::stream() << "Cache Reader No external keys found for " << _purpose
                              << " that is valid for time: " << forThisTime.toString()
                              << " with id: " << keyId};
    }

    return std::move(keys);
}

StatusWith<KeysCollectionDocument> KeysCollectionCache::getInternalKey(
    const LogicalTime& forThisTime) {
    stdx::lock_guard<stdx::mutex> lk(_cacheMutex);

    auto iter = _internalKeysCache.upper_bound(forThisTime);

    if (iter == _internalKeysCache.cend()) {
        return {ErrorCodes::KeyNotFound,
                str::stream() << "No key found that is valid for " << forThisTime.toString()};
    }

    return iter->second;
}

void KeysCollectionCache::resetCache() {
    // Refreshes try to use majority read concern, but if the client can't support that then any
    // cached keys may have been rolled back and should be cleared.
    if (_client->mustUseLocalReads()) {
        stdx::lock_guard<stdx::mutex> lk(_cacheMutex);
        _internalKeysCache.clear();
        _externalKeysCache.clear();
    }
}

void KeysCollectionCache::cacheExternalKey(ExternalKeysCollectionDocument key) {
    stdx::lock_guard<stdx::mutex> lk(_cacheMutex);
    _externalKeysCache.emplace(key.getKeyId(), std::move(key));
}

}  // namespace mongo
