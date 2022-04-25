/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include <boost/functional/hash.hpp>

#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/hasher.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/plan_cache.h"
#include "mongo/db/query/plan_cache_key_info.h"
#include "mongo/db/query/sbe_stage_builder.h"
#include "mongo/db/service_context.h"

namespace mongo {
namespace sbe {

/**
 * Represents the "key" used in the PlanCache mapping from query shape -> query plan.
 */
class PlanCacheKey {
public:
    PlanCacheKey(PlanCacheKeyInfo&& info,
                 UUID collectionUuid,
                 size_t collectionVersion,
                 boost::optional<Timestamp> newestVisibleIndexTimestamp,
                 bool isShardedCollection)
        : _info{std::move(info)},
          _collectionUuid{collectionUuid},
          _collectionVersion{collectionVersion},
          _newestVisibleIndexTimestamp{newestVisibleIndexTimestamp},
          _isShardedCollection{isShardedCollection} {}

    const UUID& getCollectionUuid() const {
        return _collectionUuid;
    }

    size_t getCollectionVersion() const {
        return _collectionVersion;
    }

    bool isShardedCollection() const {
        return _isShardedCollection;
    }

    bool operator==(const PlanCacheKey& other) const {
        return other._collectionVersion == _collectionVersion &&
            other._isShardedCollection == _isShardedCollection &&
            other._collectionUuid == _collectionUuid &&
            other._newestVisibleIndexTimestamp == _newestVisibleIndexTimestamp &&
            other._info == _info;
    }

    bool operator!=(const PlanCacheKey& other) const {
        return !(*this == other);
    }

    uint32_t queryHash() const {
        return _info.queryHash();
    }

    uint32_t planCacheKeyHash() const {
        size_t hash = _info.planCacheKeyHash();
        boost::hash_combine(hash, UUID::Hash{}(_collectionUuid));
        boost::hash_combine(hash, _collectionVersion);
        if (_newestVisibleIndexTimestamp) {
            boost::hash_combine(hash, _newestVisibleIndexTimestamp->asULL());
        }
        boost::hash_combine(hash, _isShardedCollection);
        return hash;
    }

    const std::string& toString() const {
        return _info.toString();
    }

private:
    // Contains the actual encoding of the query shape as well as the index discriminators.
    const PlanCacheKeyInfo _info;

    const UUID _collectionUuid;

    // There is a special collection versioning scheme associated with the SBE plan cache. Whenever
    // an action against a collection is made which should invalidate the plan cache entries for the
    // collection -- in particular index builds and drops -- the version number is incremented.
    // Readers specify the version number that they are reading at so that they only pick up cache
    // entries with the right set of indexes.
    //
    // We also clean up all cache entries for a particular (collectionUuid, versionNumber) pair when
    // all readers seeing this version of the collection have drained.
    const size_t _collectionVersion;

    // The '_collectionVersion' is not currently sufficient in order to ensure that the indexes
    // visible to the reader are consistent with the indexes present in the cache entry. The reason
    // is that all readers see the latest copy-on-write version of the 'Collection' object, even
    // though they are allowed to read at an older timestamp, potentially at a time before an index
    // build completed.
    //
    // To solve this problem, we incorporate the timestamp of the newest index visible to the reader
    // into the plan cache key. This ensures that the set of indexes visible to the reader match
    // those present in the plan cache entry, preventing a situation where the plan cache entry
    // reflects a newer version of the index catalog than the one visible to the reader.
    //
    // In the future, this could instead be solved with point-in-time catalog lookups.
    const boost::optional<Timestamp> _newestVisibleIndexTimestamp;

    // Ensures that a cached SBE plan cannot be reused if the collection has since become sharded.
    // The cached plan may no longer be valid after sharding, since orphan filtering is necessary
    // for sharded collections.
    const bool _isShardedCollection;
};

class PlanCacheKeyHasher {
public:
    std::size_t operator()(const PlanCacheKey& k) const {
        return k.planCacheKeyHash();
    }
};

struct PlanCachePartitioner {
    // Determines the partitioning function for use with the 'Partitioned' utility.
    std::size_t operator()(const PlanCacheKey& k, const std::size_t nPartitions) const {
        return PlanCacheKeyHasher{}(k) % nPartitions;
    }
};

/**
 * Represents the data cached in the SBE plan cache. This data holds an execution plan and necessary
 * auxiliary data for preparing and executing the PlanStage tree.
 */
struct CachedSbePlan {
    CachedSbePlan(std::unique_ptr<sbe::PlanStage> root, stage_builder::PlanStageData data)
        : root(std::move(root)), planStageData(std::move(data)) {
        tassert(5968206, "The RuntimeEnvironment should not be null", planStageData.env);
    }

    std::unique_ptr<CachedSbePlan> clone() const {
        return std::make_unique<CachedSbePlan>(root->clone(), planStageData);
    }

    uint64_t estimateObjectSizeInBytes() const {
        return root->estimateCompileTimeSize();
    }

    std::unique_ptr<sbe::PlanStage> root;
    stage_builder::PlanStageData planStageData;
    bool indexFilterApplied = false;
};

using PlanCacheEntry = PlanCacheEntryBase<CachedSbePlan, plan_cache_debug_info::DebugInfoSBE>;

struct BudgetEstimator {
    size_t operator()(const std::shared_ptr<const PlanCacheEntry>& entry) {
        return entry->estimatedEntrySizeBytes;
    }
};

using PlanCache = PlanCacheBase<PlanCacheKey,
                                CachedSbePlan,
                                BudgetEstimator,
                                plan_cache_debug_info::DebugInfoSBE,
                                PlanCachePartitioner,
                                PlanCacheKeyHasher>;

/**
 * A helper method to get the global SBE plan cache decorated in 'serviceCtx'.
 */
PlanCache& getPlanCache(ServiceContext* serviceCtx);

/**
 * A wrapper for the helper above. 'opCtx' cannot be null.
 */
PlanCache& getPlanCache(OperationContext* opCtx);

/**
 * Removes cached plan entries with the given collection UUID and collection version number.
 */
void clearPlanCacheEntriesWith(ServiceContext* serviceCtx,
                               UUID collectionUuid,
                               size_t collectionVersion);

}  // namespace sbe
}  // namespace mongo
