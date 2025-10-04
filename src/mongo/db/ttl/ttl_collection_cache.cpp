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


#include "mongo/db/ttl/ttl_collection_cache.h"

#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/decorable.h"

#include <algorithm>

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

namespace {
const auto getTTLCollectionCache = ServiceContext::declareDecoration<TTLCollectionCache>();
}

TTLCollectionCache& TTLCollectionCache::get(ServiceContext* ctx) {
    return getTTLCollectionCache(ctx);
}

void TTLCollectionCache::registerTTLInfo(UUID uuid, const Info& info) {
    {
        stdx::lock_guard<stdx::mutex> lock(_ttlInfosLock);
        _ttlInfos[uuid].push_back(info);
    }
}

void TTLCollectionCache::_deregisterTTLInfo(UUID uuid, const Info& info) {
    stdx::lock_guard<stdx::mutex> lock(_ttlInfosLock);
    auto infoIt = _ttlInfos.find(uuid);
    if (infoIt == _ttlInfos.end()) {
        LOGV2_DEBUG(9150100,
                    3,
                    "Tried to deregister index from TTLCollectionCache with untracked UUID",
                    "uuid"_attr = uuid,
                    "indexName"_attr = info.getIndexName());
        return;
    }

    auto& [_, infoVec] = *infoIt;

    auto iter = infoVec.begin();
    if (info.isClustered()) {
        // For clustered collections, we cannot have more than one clustered info per UUID.
        // All we have to do here is ensure that the 'info' to search for is also 'clustered'.
        iter = std::find_if(infoVec.begin(), infoVec.end(), [](const auto& infoVecItem) {
            return infoVecItem.isClustered();
        });
    } else {
        // For TTL indexes, we search non-clustered TTL info items on the index name only.
        auto indexName = info.getIndexName();
        iter = std::find_if(infoVec.begin(), infoVec.end(), [&indexName](const auto& infoVecItem) {
            if (infoVecItem.isClustered()) {
                return false;
            }
            return indexName == infoVecItem.getIndexName();
        });
    }

    if (iter == infoVec.end()) {
        LOGV2_DEBUG(9150101,
                    3,
                    "Tried to deregister untracked index from TTLCollectionCache",
                    "uuid"_attr = uuid,
                    "indexName"_attr = info.getIndexName());
    } else {
        infoVec.erase(iter);
    }

    if (infoVec.empty()) {
        _ttlInfos.erase(infoIt);
    }
}

void TTLCollectionCache::deregisterTTLIndexByName(UUID uuid, const IndexName& indexName) {
    _deregisterTTLInfo(std::move(uuid), TTLCollectionCache::Info{indexName, /*unusedSpec=*/{}});
}

void TTLCollectionCache::deregisterTTLClusteredIndex(UUID uuid) {
    _deregisterTTLInfo(std::move(uuid),
                       TTLCollectionCache::Info{TTLCollectionCache::ClusteredId{}});
}

void TTLCollectionCache::setTTLIndexExpireAfterSecondsType(UUID uuid,
                                                           const IndexName& indexName,
                                                           Info::ExpireAfterSecondsType type) {
    stdx::lock_guard<stdx::mutex> lock(_ttlInfosLock);
    auto infoIt = _ttlInfos.find(uuid);
    if (infoIt == _ttlInfos.end()) {
        return;
    }

    auto&& infoVec = infoIt->second;
    for (auto&& info : infoVec) {
        if (!info.isClustered() && info.getIndexName() == indexName) {
            info.setExpireAfterSecondsType(type);
            break;
        }
    }
}

TTLCollectionCache::InfoMap TTLCollectionCache::getTTLInfos() {
    stdx::lock_guard<stdx::mutex> lock(_ttlInfosLock);
    return _ttlInfos;
}
};  // namespace mongo
