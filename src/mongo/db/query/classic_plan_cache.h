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

#include <string>

#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/plan_cache.h"

namespace mongo {
/**
 * Represents the "key" used in the PlanCache mapping from query shape -> query plan.
 */
class PlanCacheKey {
public:
    PlanCacheKey(CanonicalQuery::QueryShapeString shapeString,
                 std::string indexabilityString,
                 bool enableSlotBasedExecution) {
        _lengthOfStablePart = shapeString.size();
        _key = std::move(shapeString);
        _key += indexabilityString;
        _key += enableSlotBasedExecution ? "t" : "f";
    }

    CanonicalQuery::QueryShapeString getStableKey() const {
        return std::string(_key, 0, _lengthOfStablePart);
    }

    StringData getStableKeyStringData() const {
        return StringData(_key.c_str(), _lengthOfStablePart);
    }

    /**
     * Return the 'indexability discriminators', that is, the plan cache key component after the
     * stable key, but before the boolean indicating whether we are using the classic engine.
     */
    StringData getIndexabilityDiscriminators() const {
        return StringData(_key.c_str() + _lengthOfStablePart,
                          _key.size() - _lengthOfStablePart - 1);
    }

    /**
     * Return the "unstable" portion of the key, which may vary across catalog changes.
     */
    StringData getUnstablePart() const {
        return StringData(_key.c_str() + _lengthOfStablePart, _key.size() - _lengthOfStablePart);
    }

    StringData stringData() const {
        return _key;
    }

    const std::string& toString() const {
        return _key;
    }

    bool operator==(const PlanCacheKey& other) const {
        return other._key == _key && other._lengthOfStablePart == _lengthOfStablePart;
    }

    bool operator!=(const PlanCacheKey& other) const {
        return !(*this == other);
    }

    uint32_t queryHash() const {
        return canonical_query_encoder::computeHash(getStableKeyStringData());
    }

    uint32_t planCacheKeyHash() const {
        return canonical_query_encoder::computeHash(stringData());
    }

private:
    // Key is broken into three parts:
    // <stable key> | <indexability discriminators> | <enableSlotBasedExecution boolean>
    // This third part can be removed once the classic query engine reaches EOL and SBE is used
    // exclusively for all query execution. Combined, the three parts make up the plan cache key.
    // We store them in one std::string so that we can easily/cheaply extract the stable key.
    std::string _key;

    // How long the "stable key" is.
    size_t _lengthOfStablePart;
};

std::ostream& operator<<(std::ostream& stream, const PlanCacheKey& key);
StringBuilder& operator<<(StringBuilder& builder, const PlanCacheKey& key);

class PlanCacheKeyHasher {
public:
    std::size_t operator()(const PlanCacheKey& k) const {
        return std::hash<std::string>{}(k.toString());
    }
};

/**
 * A PlanCacheIndexTree is the meaty component of the data
 * stored in SolutionCacheData. It is a tree structure with
 * index tags that indicates to the access planner which indices
 * it should try to use.
 *
 * How a PlanCacheIndexTree is created:
 *   The query planner tags a match expression with indices. It
 *   then uses the tagged tree to create a PlanCacheIndexTree,
 *   using QueryPlanner::cacheDataFromTaggedTree. The PlanCacheIndexTree
 *   is isomorphic to the tagged match expression, and has matching
 *   index tags.
 *
 * How a PlanCacheIndexTree is used:
 *   When the query planner is planning from the cache, it uses
 *   the PlanCacheIndexTree retrieved from the cache in order to
 *   recreate index assignments. Specifically, a raw MatchExpression
 *   is tagged according to the index tags in the PlanCacheIndexTree.
 *   This is done by QueryPlanner::tagAccordingToCache.
 */
struct PlanCacheIndexTree {
    /**
     * An OrPushdown is the cached version of an OrPushdownTag::Destination. It indicates that this
     * node is a predicate that can be used inside of a sibling indexed OR, to tighten index bounds
     * or satisfy the first field in the index.
     */
    struct OrPushdown {
        uint64_t estimateObjectSizeInBytes() const {
            return  // Add size of each element in 'route' vector.
                container_size_helper::estimateObjectSizeInBytes(route) +
                // Subtract static size of 'identifier' since it is already included in
                // 'sizeof(*this)'.
                (indexEntryId.estimateObjectSizeInBytes() - sizeof(indexEntryId)) +
                // Add size of the object.
                sizeof(*this);
        }
        IndexEntry::Identifier indexEntryId;
        size_t position;
        bool canCombineBounds;
        std::deque<size_t> route;
    };

    PlanCacheIndexTree() : entry(nullptr), index_pos(0), canCombineBounds(true) {}

    /**
     * Clone 'ie' and set 'this->entry' to be the clone.
     */
    void setIndexEntry(const IndexEntry& ie);

    /**
     * Make a deep copy.
     */
    std::unique_ptr<PlanCacheIndexTree> clone() const;

    /**
     * For debugging.
     */
    std::string toString(int indents = 0) const;

    uint64_t estimateObjectSizeInBytes() const {
        return  // Recursively add size of each element in 'children' vector.
            container_size_helper::estimateObjectSizeInBytes(
                children,
                [](const auto& child) { return child->estimateObjectSizeInBytes(); },
                true) +
            // Add size of each element in 'orPushdowns' vector.
            container_size_helper::estimateObjectSizeInBytes(
                orPushdowns,
                [](const auto& orPushdown) { return orPushdown.estimateObjectSizeInBytes(); },
                false) +
            // Add size of 'entry' if present.
            (entry ? entry->estimateObjectSizeInBytes() : 0) +
            // Add size of the object.
            sizeof(*this);
    }

    std::vector<std::unique_ptr<PlanCacheIndexTree>> children;

    // Owned here.
    std::unique_ptr<IndexEntry> entry;

    size_t index_pos;

    // The value for this member is taken from the IndexTag of the corresponding match expression
    // and is used to ensure that bounds are correctly intersected and/or compounded when a query is
    // planned from the plan cache.
    bool canCombineBounds;

    std::vector<OrPushdown> orPushdowns;
};

/**
 * Data stored inside a QuerySolution which can subsequently be used to create a cache entry. When
 * this data is retrieved from the cache, it is sufficient to reconstruct the original
 * QuerySolution.
 */
struct SolutionCacheData {
    SolutionCacheData()
        : tree(nullptr),
          solnType(USE_INDEX_TAGS_SOLN),
          wholeIXSolnDir(1),
          indexFilterApplied(false) {}

    std::unique_ptr<SolutionCacheData> clone() const;

    // For debugging.
    std::string toString() const;

    uint64_t estimateObjectSizeInBytes() const {
        return (tree ? tree->estimateObjectSizeInBytes() : 0) + sizeof(*this);
    }

    // If 'wholeIXSoln' is false, then 'tree' can be used to tag an isomorphic match expression.
    // If 'wholeIXSoln' is true, then 'tree' is used to store the relevant IndexEntry.
    // If 'collscanSoln' is true, then 'tree' should be NULL.
    std::unique_ptr<PlanCacheIndexTree> tree;

    enum SolutionType {
        // Indicates that the plan should use
        // the index as a proxy for a collection
        // scan (e.g. using index to provide sort).
        WHOLE_IXSCAN_SOLN,

        // The cached plan is a collection scan.
        COLLSCAN_SOLN,

        // Build the solution by using 'tree'
        // to tag the match expression.
        USE_INDEX_TAGS_SOLN
    } solnType;

    // The direction of the index scan used as
    // a proxy for a collection scan. Used only
    // for WHOLE_IXSCAN_SOLN.
    int wholeIXSolnDir;

    // True if index filter was applied.
    bool indexFilterApplied;
};

using PlanCacheEntry = PlanCacheEntryBase<SolutionCacheData>;

using CachedSolution = CachedPlanHolder<SolutionCacheData>;

using PlanCache = PlanCacheBase<PlanCacheKey, SolutionCacheData, PlanCacheKeyHasher>;

}  // namespace mongo
