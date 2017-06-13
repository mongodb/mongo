/**
 *    Copyright (C) 2017 MongoDB, Inc.
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

#include "mongo/db/keys_collection_cache_reader.h"

#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

KeysCollectionCacheReader::KeysCollectionCacheReader(std::string purpose,
                                                     ShardingCatalogClient* client)
    : _purpose(std::move(purpose)), _catalogClient(client) {}

StatusWith<KeysCollectionDocument> KeysCollectionCacheReader::refresh(OperationContext* opCtx) {
    LogicalTime newerThanThis;

    {
        stdx::lock_guard<stdx::mutex> lk(_cacheMutex);
        auto iter = _cache.crbegin();
        if (iter != _cache.crend()) {
            newerThanThis = iter->second.getExpiresAt();
        }
    }

    auto refreshStatus = _catalogClient->getNewKeys(
        opCtx, _purpose, newerThanThis, repl::ReadConcernLevel::kMajorityReadConcern);

    if (!refreshStatus.isOK()) {
        return refreshStatus.getStatus();
    }

    auto& newKeys = refreshStatus.getValue();

    stdx::lock_guard<stdx::mutex> lk(_cacheMutex);
    for (auto&& key : newKeys) {
        _cache.emplace(std::make_pair(key.getExpiresAt(), std::move(key)));
    }

    if (_cache.empty()) {
        return {ErrorCodes::KeyNotFound, "No keys found after refresh"};
    }

    return _cache.crbegin()->second;
}

StatusWith<KeysCollectionDocument> KeysCollectionCacheReader::getKeyById(
    long long keyId, const LogicalTime& forThisTime) {
    stdx::lock_guard<stdx::mutex> lk(_cacheMutex);

    for (auto iter = _cache.lower_bound(forThisTime); iter != _cache.cend(); ++iter) {
        if (iter->second.getKeyId() == keyId) {
            return iter->second;
        }
    }

    return {ErrorCodes::KeyNotFound,
            str::stream() << "No keys found for " << _purpose << " that is valid for time: "
                          << forThisTime.toString()
                          << " with id: "
                          << keyId};
}

StatusWith<KeysCollectionDocument> KeysCollectionCacheReader::getKey(
    const LogicalTime& forThisTime) {
    stdx::lock_guard<stdx::mutex> lk(_cacheMutex);

    auto iter = _cache.upper_bound(forThisTime);

    if (iter == _cache.cend()) {
        return {ErrorCodes::KeyNotFound,
                str::stream() << "No key found that is valid for " << forThisTime.toString()};
    }

    return iter->second;
}

}  // namespace mongo
