// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
        std::lock_guard<std::mutex> lk(_cacheMutex);
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

    std::lock_guard<std::mutex> lk(_cacheMutex);
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
        std::lock_guard<std::mutex> lk(_cacheMutex);
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

    std::lock_guard<std::mutex> lk(_cacheMutex);
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
    std::lock_guard<std::mutex> lk(_cacheMutex);

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
    std::lock_guard<std::mutex> lk(_cacheMutex);
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
    std::lock_guard<std::mutex> lk(_cacheMutex);

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
        std::lock_guard<std::mutex> lk(_cacheMutex);
        _internalKeysCache.clear();
        _externalKeysCache.clear();
    }
}

void KeysCollectionCache::cacheExternalKey(ExternalKeysCollectionDocument key) {
    std::lock_guard<std::mutex> lk(_cacheMutex);
    _externalKeysCache.emplace(key.getKeyId(), std::move(key));
}

}  // namespace mongo
