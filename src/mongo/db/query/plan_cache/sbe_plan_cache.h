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

#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/hasher.h"
#include "mongo/db/local_catalog/util/partitioned.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/plan_cache/plan_cache.h"
#include "mongo/db/query/plan_cache/plan_cache_debug_info.h"
#include "mongo/db/query/plan_cache/plan_cache_key_info.h"
#include "mongo/db/query/stage_builder/sbe/builder_data.h"
#include "mongo/db/service_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/container_size_helper.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/functional/hash.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace sbe {

/**
 * Represents the sharding epoch of the collection which entry will be stored in the plan cache. The
 * sharding epoch is not updated on operations like chunk splits and moves but rather on sharding
 * and refine shard key operations.
 */
struct PlanCacheKeyShardingEpoch {
    bool operator==(const PlanCacheKeyShardingEpoch& other) const {
        return epoch == other.epoch && ts == other.ts;
    }

    OID epoch;
    Timestamp ts;
};

struct PlanCacheKeyCollectionState {
    bool operator==(const PlanCacheKeyCollectionState& other) const {
        return other.uuid == uuid && other.version == version &&
            other.collectionGeneration == collectionGeneration;
    }

    size_t hashCode() const {
        size_t hash = UUID::Hash{}(uuid);
        boost::hash_combine(hash, version);
        if (collectionGeneration) {
            collectionGeneration->epoch.hash_combine(hash);
            boost::hash_combine(hash, collectionGeneration->ts.asULL());
        }
        return hash;
    }

    UUID uuid;

    // There is a special collection versioning scheme associated with the SBE plan cache. Whenever
    // an action against a collection is made which should invalidate the plan cache entries for the
    // collection -- in particular index builds and drops -- the version number is incremented.
    // Readers specify the version number that they are reading at so that they only pick up cache
    // entries with the right set of indexes.
    //
    // We also clean up all cache entries for a particular (collectionUuid, versionNumber) pair when
    // all readers seeing this version of the collection have drained.
    size_t version;

    // Ensures that a cached SBE plan cannot be reused if the collection has since become sharded or
    // changed its shard key. The cached plan may no longer be valid after sharding or shard key
    // refining since the structure of the plan depends on whether the collection is sharded, and if
    // sharded depends on the shard key.
    const boost::optional<PlanCacheKeyShardingEpoch> collectionGeneration;
};

/**
 * Represents the "key" used in the PlanCache mapping from query shape -> query plan.
 */
class PlanCacheKey {
public:
    PlanCacheKey(PlanCacheKeyInfo&& info,
                 PlanCacheKeyCollectionState mainCollectionState,
                 std::vector<PlanCacheKeyCollectionState> secondaryCollectionStates)
        : _info{std::move(info)},
          _mainCollectionState{std::move(mainCollectionState)},
          _secondaryCollectionStates{std::move(secondaryCollectionStates)} {
        // For secondary collections, we don't encode collection generation in the key since we
        // don't shard version these collections. This is OK because we only push down $lookup
        // queries to SBE when involved collections are unsharded.
        for (const auto& collState : _secondaryCollectionStates) {
            tassert(
                6443202,
                "Secondary collections should not encode collection generation in plan cache key",
                collState.collectionGeneration == boost::none);
        }
    }

    const PlanCacheKeyCollectionState& getMainCollectionState() const {
        return _mainCollectionState;
    }

    const std::vector<PlanCacheKeyCollectionState>& getSecondaryCollectionStates() const {
        return _secondaryCollectionStates;
    }

    bool operator==(const PlanCacheKey& other) const {
        return other._info == _info && other._mainCollectionState == _mainCollectionState &&
            other._secondaryCollectionStates == _secondaryCollectionStates;
    }

    bool operator!=(const PlanCacheKey& other) const {
        return !(*this == other);
    }

    uint32_t planCacheShapeHash() const {
        return _info.planCacheShapeHash();
    }

    uint32_t planCacheKeyHash() const {
        size_t hash = _info.planCacheKeyHash();
        boost::hash_combine(hash, _mainCollectionState.hashCode());
        for (auto& collectionState : _secondaryCollectionStates) {
            boost::hash_combine(hash, collectionState.hashCode());
        }
        return hash;
    }

    const std::string& toString() const {
        return _info.toString();
    }

    /**
     * Returns the estimated size of the plan cache key in bytes.
     */
    uint64_t estimatedKeySizeBytes() const {
        return sizeof(*this) + _info.keySizeInBytes() +
            container_size_helper::estimateObjectSizeInBytes(_secondaryCollectionStates);
    }

    const query_settings::QuerySettings& querySettings() const {
        return _info.querySettings();
    }

private:
    // Contains the actual encoding of the query shape as well as the index discriminators.
    const PlanCacheKeyInfo _info;

    const PlanCacheKeyCollectionState _mainCollectionState;

    // To make sure the plan cache key matches, the secondary collection states need to be passed
    // in a defined order. Currently, we use the collection order stored in
    // MultipleCollectionAccessor, which is ordered by the collection namespaces.
    const std::vector<PlanCacheKeyCollectionState> _secondaryCollectionStates;
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
 * auxiliary data for preparing and executing the PlanStage tree. In addition, holds the hash of the
 * query solution that led to this plan.
 */
struct CachedSbePlan {
    CachedSbePlan(std::unique_ptr<sbe::PlanStage> root,
                  stage_builder::PlanStageData data,
                  size_t hash)
        : root(std::move(root)), planStageData(std::move(data)), solutionHash(hash) {
        tassert(5968206, "The RuntimeEnvironment should not be null", planStageData.env.runtimeEnv);
    }

    std::unique_ptr<CachedSbePlan> clone() const {
        return std::make_unique<CachedSbePlan>(root->clone(), planStageData, solutionHash);
    }

    uint64_t estimateObjectSizeInBytes() const {
        return root->estimateCompileTimeSize();
    }

    std::unique_ptr<sbe::PlanStage> root;
    stage_builder::PlanStageData planStageData;
    bool indexFilterApplied = false;
    // Hash of the QuerySolution that led this cache entry.
    size_t solutionHash;
};

using PlanCacheEntry = PlanCacheEntryBase<CachedSbePlan, plan_cache_debug_info::DebugInfoSBE>;
using CachedPlanHolder =
    mongo::CachedPlanHolder<CachedSbePlan, plan_cache_debug_info::DebugInfoSBE>;

struct BudgetEstimator {
    /**
     * This estimator function is called when an entry is added or removed to LRU cache in order to
     * make sure the total plan cache size does not exceed the maximum size.
     */
    size_t operator()(const sbe::PlanCacheKey& key,
                      const std::shared_ptr<const PlanCacheEntry>& entry) {
        return entry->estimatedEntrySizeBytes + key.estimatedKeySizeBytes();
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
 *
 * When 'matchSecondaryCollections' is 'true' this function will also clear plan cache entries
 * whose list of secondary collection contains at least one collection with the 'collectionUuid'.
 * Otherwise, only the main collection will be matched against the given 'collectionUuid'.
 */
void clearPlanCacheEntriesWith(ServiceContext* serviceCtx,
                               UUID collectionUuid,
                               size_t collectionVersion,
                               bool matchSecondaryCollections);

}  // namespace sbe
}  // namespace mongo
