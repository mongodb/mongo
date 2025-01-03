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

#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/plan_ranking_decision.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/container_size_helper.h"

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
