// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


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
        std::lock_guard<std::mutex> lock(_ttlInfosLock);
        _ttlInfos[uuid].push_back(info);
    }
}

void TTLCollectionCache::_deregisterTTLInfo(UUID uuid, const Info& info) {
    std::lock_guard<std::mutex> lock(_ttlInfosLock);
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
    std::lock_guard<std::mutex> lock(_ttlInfosLock);
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
    std::lock_guard<std::mutex> lock(_ttlInfosLock);
    return _ttlInfos;
}
};  // namespace mongo
