/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/keys_collection_manager_direct.h"

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"

namespace mongo {

namespace {
const char kLogicalTimeKeysCollection[] = "admin.system.keys";
const int kMaxCachedKeys = 20;
}  // namespace

KeysCollectionManagerDirect::KeysCollectionManagerDirect(std::string purpose,
                                                         Seconds keyValidForInterval)
    : _purpose(std::move(purpose)),
      _keyValidForInterval(keyValidForInterval),
      _cache(kMaxCachedKeys) {}

StatusWith<KeysCollectionDocument> KeysCollectionManagerDirect::getKeyForValidation(
    OperationContext* opCtx, long long keyId, const LogicalTime& forThisTime) {
    // First, attempt to find the key in our cache.
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        auto it = _cache.find(keyId);
        if (it != _cache.end()) {
            return it->second;
        }
    }

    // Query admin.system.keys for an active key with this id.
    DBDirectClient client(opCtx);

    BSONObjBuilder queryBuilder;
    queryBuilder.append("purpose", _purpose);
    queryBuilder.append("_id", keyId);
    queryBuilder.append("expiresAt", BSON("$gt" << forThisTime.asTimestamp()));

    auto cursor = client.query(KeysCollectionDocument::ConfigNS, queryBuilder.obj());

    if (!cursor->more()) {
        return {ErrorCodes::KeyNotFound, "Could not find matching key"};
    }

    // Parse the key.
    auto res = KeysCollectionDocument::fromBSON(cursor->next());
    if (!res.isOK()) {
        return res.getStatus();
    }

    // Add to our cache.
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _cache.add(keyId, res.getValue());
    }

    return res.getValue();
}

StatusWith<KeysCollectionDocument> KeysCollectionManagerDirect::getKeyForSigning(
    OperationContext* opCtx, const LogicalTime& forThisTime) {
    // Search through the cache for active keys.
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        for (auto& it : _cache) {
            auto keyDoc = it.second;
            auto expiration = keyDoc.getExpiresAt();
            if (expiration > forThisTime) {
                return keyDoc;
            }
        }
    }

    // Query admin.system.keys for active keys.
    DBDirectClient client(opCtx);

    BSONObjBuilder queryBuilder;
    queryBuilder.append("purpose", _purpose);
    queryBuilder.append("expiresAt", BSON("$gt" << forThisTime.asTimestamp()));

    auto cursor = client.query(KeysCollectionDocument::ConfigNS, queryBuilder.obj());

    if (!cursor->more()) {
        return {ErrorCodes::KeyNotFound, "Could not find an active key for signing"};
    }

    // Parse and return the key.
    auto res = KeysCollectionDocument::fromBSON(cursor->next());
    if (!res.isOK()) {
        return res.getStatus();
    }

    auto keyDoc = res.getValue();

    // Add to our cache.
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _cache.add(keyDoc.getKeyId(), keyDoc);
    }

    return keyDoc;
}

}  // namespace mongo
