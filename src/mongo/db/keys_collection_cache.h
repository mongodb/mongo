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

#pragma once

#include <map>

#include "mongo/base/status_with.h"
#include "mongo/db/keys_collection_document_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/platform/mutex.h"

namespace mongo {

class KeysCollectionClient;

/**
 * Keeps a local cache of the keys with the ability to refresh.
 *
 * Note: This assumes that user does not manually update the keys collection.
 */
class KeysCollectionCache {
public:
    KeysCollectionCache(std::string purpose, KeysCollectionClient* client);
    ~KeysCollectionCache() = default;

    /**
     * Check if there are new documents expiresAt > latestKeyDoc.expiresAt.
     */
    StatusWith<KeysCollectionDocument> refresh(OperationContext* opCtx);

    /**
     * Returns the internal key (see definition below) with an expiresAt value greater than
     * forThisTime. Returns KeyNotFound if there is no such key.
     */
    StatusWith<KeysCollectionDocument> getInternalKey(const LogicalTime& forThisTime);

    /**
     * Returns the internal key (see definition below) with the given keyId and an expiresAt value
     * greater than forThisTime. There should only be one matching key since keyId is unique for
     * keys generated within a cluster. Returns KeyNotFound if there is no such key.
     */
    StatusWith<KeysCollectionDocument> getInternalKeyById(long long keyId,
                                                          const LogicalTime& forThisTime);

    /**
     * Returns the external keys (see definition below) with the given keyId and an expiresAt value
     * greater than forThisTime. There are a variable number of matching keys since keyId is not
     * necessarily unique across clusters. Returns KeyNotFound if there are no such keys.
     */
    StatusWith<std::vector<ExternalKeysCollectionDocument>> getExternalKeysById(
        long long keyId, const LogicalTime& forThisTime);

    /**
     * Resets the cache of keys if the client doesnt allow readConcern level:majority reads.
     * This method intended to be called on the rollback of the node.
     */
    void resetCache();

private:
    /**
     * Checks if there are new internal key documents (see definition below) with expiresAt greater
     * than the latest internal key document's expiresAt. Returns KeyNotFound if _internalKeysCache
     * is empty after refresh.
     */
    StatusWith<KeysCollectionDocument> _refreshInternalKeys(OperationContext* opCtx);

    /**
     * Checks if there are new external key documents (see definition below). Does not return
     * KeyNotFound if _externalKeysCache is empty after refresh.
     */
    Status _refreshExternalKeys(OperationContext* opCtx);

    const std::string _purpose;
    KeysCollectionClient* const _client;

    Mutex _cacheMutex = MONGO_MAKE_LATCH("KeysCollectionCache::_cacheMutex");

    // Stores keys for signing and validating cluster times created by the cluster that this node
    // is in.
    std::map<LogicalTime, KeysCollectionDocument> _internalKeysCache;  // expiresAt -> KeysDocument

    // Stores keys for validating cluster times created by other clusters. These key documents
    // cannot be stored in a regular map like _internalKeysCache since expiresAt and keyId are not
    // necessarily unique across clusters so there is chance of collision.
    stdx::unordered_map<long long, StringMap<ExternalKeysCollectionDocument>>
        _externalKeysCache;  // keyId -> (replicaSetName -> ExternalKeysDocument)
};

}  // namespace mongo
