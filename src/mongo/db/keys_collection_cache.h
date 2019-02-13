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
#include "mongo/db/keys_collection_document.h"
#include "mongo/db/operation_context.h"
#include "mongo/stdx/mutex.h"

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

    StatusWith<KeysCollectionDocument> getKey(const LogicalTime& forThisTime);
    StatusWith<KeysCollectionDocument> getKeyById(long long keyId, const LogicalTime& forThisTime);

    /**
     * Resets the cache of keys if the client doesnt allow readConcern level:majority reads.
     * This method intended to be called on the rollback of the node.
     */
    void resetCache();

private:
    const std::string _purpose;
    KeysCollectionClient* const _client;

    stdx::mutex _cacheMutex;
    std::map<LogicalTime, KeysCollectionDocument> _cache;  // expiresAt -> KeysDocument
};

}  // namespace mongo
