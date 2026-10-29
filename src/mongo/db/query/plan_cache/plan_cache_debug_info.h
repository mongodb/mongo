// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/container_size_helper.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/plan_ranking_decision.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/modules.h"

namespace mongo::plan_cache_debug_info {
/**
 * A description of the query from which a paln cache entry was created.
 */
struct CreatedFromQuery {
    /**
     * Returns an estimate of the size of this object, including the memory allocated elsewhere
     * that it owns, in bytes.
     */
    uint64_t estimateObjectSizeInBytes() const {
        uint64_t size = 0;
        size += filter.objsize();
        size += sort.objsize();
        size += projection.objsize();
        size += collation.objsize();
        size += distinct.objsize();
        return size;
    }

    std::string debugString() const {
        return str::stream() << "query: " << filter.toString() << "; sort: " << sort.toString()
                             << "; projection: " << projection.toString()
                             << "; collation: " << collation.toString()
                             << "; distinct: " << distinct.toString();
    }

    BSONObj filter;
    BSONObj sort;
    BSONObj projection;
    BSONObj collation;
    BSONObj distinct;
};

/**
 * Per-plan cache entry information that is used strictly as debug information (e.g. is intended
 * for display by the $planCacheStats aggregation source). In order to save memory, this
 * information is sometimes discarded instead of kept in the plan cache entry. Therefore, this
 * information may not be used for any purpose outside displaying debug info, such as recovering
 * a plan from the cache or determining whether or not the cache entry is active.
 */
struct DebugInfo {
    DebugInfo(CreatedFromQuery createdFromQuery,
              std::unique_ptr<const plan_ranker::PlanRankingDecision> decision)
        : createdFromQuery(std::move(createdFromQuery)), decision(std::move(decision)) {
        invariant(this->decision);
    }

    /**
     * 'DebugInfo' is copy-constructible, copy-assignable, move-constructible, and
     * move-assignable.
     */
    DebugInfo(const DebugInfo& other)
        : createdFromQuery(other.createdFromQuery), decision(other.decision->clone()) {}

    DebugInfo& operator=(const DebugInfo& other) {
        if (this == &other) {
            return *this;
        }
        createdFromQuery = other.createdFromQuery;
        decision = other.decision->clone();
        return *this;
    }

    DebugInfo(DebugInfo&&) = default;
    DebugInfo& operator=(DebugInfo&&) = default;

    ~DebugInfo() = default;

    /**
     * Returns an estimate of the size of this object, including the memory allocated elsewhere
     * that it owns, in bytes.
     */
    uint64_t estimateObjectSizeInBytes() const {
        uint64_t size = 0;
        size += createdFromQuery.estimateObjectSizeInBytes();
        size += decision->estimateObjectSizeInBytes();
        return size;
    }

    std::string debugString() const {
        return createdFromQuery.debugString();
    }

    CreatedFromQuery createdFromQuery;

    // Information that went into picking the winning plan and also why the other plans lost.
    // Never nullptr.
    std::unique_ptr<const plan_ranker::PlanRankingDecision> decision;
};

struct CollectionDebugInfoSBE {
    long long collectionScans = 0;
    long long collectionScansNonTailable = 0;
    std::vector<std::string> indexesUsed;
};

/*
 * Similar to "DebugInfo" above. This debug info struct is only for SBE plan cache.
 */
struct DebugInfoSBE {
    uint64_t estimateObjectSizeInBytes() const {
        uint64_t size = sizeof(DebugInfoSBE) + planSummary.capacity();
        size += container_size_helper::estimateObjectSizeInBytes(
            mainStats.indexesUsed, [](std::string str) { return str.capacity(); }, true);
        for (auto& [_, stats] : secondaryStats) {
            size += container_size_helper::estimateObjectSizeInBytes(
                stats.indexesUsed, [](std::string str) { return str.capacity(); }, true);
        }
        return size;
    }

    std::string debugString() const {
        return planSummary;
    }

    CollectionDebugInfoSBE mainStats;
    mongo::stdx::unordered_map<NamespaceString, CollectionDebugInfoSBE> secondaryStats;
    std::string planSummary;
};
}  // namespace mongo::plan_cache_debug_info
