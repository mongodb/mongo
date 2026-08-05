// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/service_context.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <mutex>
#include <string>
#include <utility>
#include <variant>
#include <vector>

/**
 * Caches the set of collections containing a TTL index.
 * This class is thread safe.
 */
namespace [[MONGO_MOD_PUBLIC]] mongo {

class TTLCollectionCache {
public:
    static TTLCollectionCache& get(ServiceContext* ctx);

    // Specifies that a collection is clustered and is TTL.
    class ClusteredId : public std::monostate {};
    // Names an index that is TTL.
    using IndexName = std::string;

    // Specifies how a collection should expire data with TTL.
    class Info {
    public:
        enum class ExpireAfterSecondsType { kInvalid, kNonInt, kInt };

        explicit Info(ClusteredId)
            : _isClustered(true), _expireAfterSecondsType(ExpireAfterSecondsType::kInt) {}
        Info(IndexName indexName, ExpireAfterSecondsType type)
            : _isClustered(false),
              _indexName(std::move(indexName)),
              _expireAfterSecondsType(type) {}
        bool isClustered() const {
            return _isClustered;
        }
        IndexName getIndexName() const {
            return _indexName;
        }
        bool isExpireAfterSecondsInvalid() const {
            return _expireAfterSecondsType == ExpireAfterSecondsType::kInvalid;
        }
        bool isExpireAfterSecondsNonInt() const {
            return _expireAfterSecondsType == ExpireAfterSecondsType::kNonInt;
        }
        void setExpireAfterSecondsType(ExpireAfterSecondsType type) {
            _expireAfterSecondsType = type;
        }

    private:
        bool _isClustered;
        IndexName _indexName;
        ExpireAfterSecondsType _expireAfterSecondsType;
    };

    void registerTTLInfo(UUID uuid, const Info& info);
    void deregisterTTLIndexByName(UUID uuid, const IndexName& indexName);
    void deregisterTTLClusteredIndex(UUID uuid);

    /**
     * Resets expireAfterSeconds flag on TTL index.
     * For idempotency, this has no effect if index is not found.
     */
    void setTTLIndexExpireAfterSecondsType(UUID uuid,
                                           const IndexName& indexName,
                                           Info::ExpireAfterSecondsType type);

    using InfoMap = stdx::unordered_map<UUID, std::vector<Info>, UUID::Hash>;
    [[MONGO_MOD_PRIVATE]] InfoMap getTTLInfos();

private:
    /**
     * Shared implementation for deregistering TTL infos.
     */
    void _deregisterTTLInfo(UUID uuid, const Info& info);
    void _deregisterTTLInfo_inlock(UUID uuid, const Info& info);

    std::mutex _ttlInfosLock;
    InfoMap _ttlInfos;
};
}  // namespace mongo
