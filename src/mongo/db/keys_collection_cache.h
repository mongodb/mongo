// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/keys_collection_document_gen.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

#include <map>
#include <mutex>
#include <string>
#include <vector>

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

    /**
     * Loads the given external key into the cache.
     */
    void cacheExternalKey(ExternalKeysCollectionDocument key);

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

    std::mutex _cacheMutex;

    // Stores keys for signing and validating cluster times created by the cluster that this node
    // is in.
    std::map<LogicalTime, KeysCollectionDocument> _internalKeysCache;  // expiresAt -> KeysDocument

    // Stores keys for validating cluster times created by other clusters. These key documents
    // cannot be stored in a regular map like _internalKeysCache since expiresAt and keyId are not
    // necessarily unique across clusters so there is chance of collision.
    std::multimap<long long, ExternalKeysCollectionDocument> _externalKeysCache;
};

}  // namespace mongo
