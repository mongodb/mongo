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

#pragma once

#include <boost/optional/optional.hpp>
#include <set>

#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/canonical_query_encoder.h"
#include "mongo/db/query/index_tag.h"
#include "mongo/db/query/lru_key_value.h"
#include "mongo/db/query/plan_cache_indexability.h"
#include "mongo/db/query/plan_ranking_decision.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/util/container_size_helper.h"

namespace mongo {
// The logging facility enforces the rule that logging should not be done in a header file. Since
// template classes and functions below must be defined in the header file and since they use the
// logging facility, we have to define the helper functions below to perform the actual logging
// operation from template code.
namespace log_detail {
void logInactiveCacheEntry(const std::string& key);
void logCacheEviction(NamespaceString nss, std::string&& evictedEntry);
void logCreateInactiveCacheEntry(std::string&& query,
                                 std::string&& queryHash,
                                 std::string&& planCacheKey,
                                 size_t newWorks);
void logReplaceActiveCacheEntry(std::string&& query,
                                std::string&& queryHash,
                                std::string&& planCacheKey,
                                size_t works,
                                size_t newWorks);
void logNoop(std::string&& query,
             std::string&& queryHash,
             std::string&& planCacheKey,
             size_t works,
             size_t newWorks);
void logIncreasingWorkValue(std::string&& query,
                            std::string&& queryHash,
                            std::string&& planCacheKey,
                            size_t works,
                            size_t increasedWorks);
void logPromoteCacheEntry(std::string&& query,
                          std::string&& queryHash,
                          std::string&& planCacheKey,
                          size_t works,
                          size_t newWorks);
}  // namespace log_detail

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

class QuerySolution;
struct QuerySolutionNode;

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

    ~PlanCacheIndexTree() {
        for (std::vector<PlanCacheIndexTree*>::const_iterator it = children.begin();
             it != children.end();
             ++it) {
            delete *it;
        }
    }

    /**
     * Clone 'ie' and set 'this->entry' to be the clone.
     */
    void setIndexEntry(const IndexEntry& ie) {
        entry.reset(new IndexEntry(ie));
    }

    /**
     * Make a deep copy.
     */
    PlanCacheIndexTree* clone() const {
        PlanCacheIndexTree* root = new PlanCacheIndexTree();
        if (nullptr != entry.get()) {
            root->index_pos = index_pos;
            root->setIndexEntry(*entry.get());
            root->canCombineBounds = canCombineBounds;
        }
        root->orPushdowns = orPushdowns;

        for (std::vector<PlanCacheIndexTree*>::const_iterator it = children.begin();
             it != children.end();
             ++it) {
            PlanCacheIndexTree* clonedChild = (*it)->clone();
            root->children.push_back(clonedChild);
        }
        return root;
    }

    /**
     * For debugging.
     */
    std::string toString(int indents = 0) const {
        StringBuilder result;
        if (!children.empty()) {
            result << std::string(3 * indents, '-') << "Node\n";
            int newIndent = indents + 1;
            for (std::vector<PlanCacheIndexTree*>::const_iterator it = children.begin();
                 it != children.end();
                 ++it) {
                result << (*it)->toString(newIndent);
            }
            return result.str();
        } else {
            result << std::string(3 * indents, '-') << "Leaf ";
            if (nullptr != entry.get()) {
                result << entry->identifier << ", pos: " << index_pos << ", can combine? "
                       << canCombineBounds;
            }
            for (const auto& orPushdown : orPushdowns) {
                result << "Move to ";
                bool firstPosition = true;
                for (auto position : orPushdown.route) {
                    if (!firstPosition) {
                        result << ",";
                    }
                    firstPosition = false;
                    result << position;
                }
                result << ": " << orPushdown.indexEntryId << " pos: " << orPushdown.position
                       << ", can combine? " << orPushdown.canCombineBounds << ". ";
            }
            result << '\n';
        }
        return result.str();
    }

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
    // Children owned here.
    std::vector<PlanCacheIndexTree*> children;

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
 * Data stored inside a QuerySolution which can subsequently be
 * used to create a cache entry. When this data is retrieved
 * from the cache, it is sufficient to reconstruct the original
 * QuerySolution.
 */
struct SolutionCacheData {
    SolutionCacheData()
        : tree(nullptr),
          solnType(USE_INDEX_TAGS_SOLN),
          wholeIXSolnDir(1),
          indexFilterApplied(false) {}

    std::unique_ptr<SolutionCacheData> clone() const {
        auto other = std::make_unique<SolutionCacheData>();
        if (nullptr != this->tree.get()) {
            // 'tree' could be NULL if the cached solution
            // is a collection scan.
            other->tree.reset(this->tree->clone());
        }
        other->solnType = this->solnType;
        other->wholeIXSolnDir = this->wholeIXSolnDir;
        other->indexFilterApplied = this->indexFilterApplied;
        return other;
    }

    // For debugging.
    std::string toString() const {
        switch (this->solnType) {
            case WHOLE_IXSCAN_SOLN:
                verify(this->tree.get());
                return str::stream() << "(whole index scan solution: "
                                     << "dir=" << this->wholeIXSolnDir << "; "
                                     << "tree=" << this->tree->toString() << ")";
            case COLLSCAN_SOLN:
                return "(collection scan)";
            case USE_INDEX_TAGS_SOLN:
                verify(this->tree.get());
                return str::stream() << "(index-tagged expression tree: "
                                     << "tree=" << this->tree->toString() << ")";
        }
        MONGO_UNREACHABLE;
    }

    uint64_t estimateObjectSizeInBytes() const {
        return (tree ? tree->estimateObjectSizeInBytes() : 0) + sizeof(*this);
    }

    // Owned here. If 'wholeIXSoln' is false, then 'tree'
    // can be used to tag an isomorphic match expression. If 'wholeIXSoln'
    // is true, then 'tree' is used to store the relevant IndexEntry.
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

template <class CachedPlanType>
class PlanCacheEntryBase;

/**
 * Information returned from a get(...) query.
 */
template <class CachedPlanType>
class CachedPlanHolder {
private:
    CachedPlanHolder(const CachedPlanHolder&) = delete;
    CachedPlanHolder& operator=(const CachedPlanHolder&) = delete;

public:
    CachedPlanHolder(const PlanCacheEntryBase<CachedPlanType>& entry)
        : cachedPlan(entry.cachedPlan->clone()), decisionWorks(entry.works) {}


    // A cached plan that can be used to reconstitute the complete execution plan from cache.
    std::unique_ptr<CachedPlanType> cachedPlan;

    // The number of work cycles taken to decide on a winning plan when the plan was first
    // cached.
    const size_t decisionWorks;
};

using CachedSolution = CachedPlanHolder<SolutionCacheData>;

/**
 * Used by the cache to track entries and their performance over time.
 * Also used by the plan cache commands to display plan cache state.
 */
template <class CachedPlanType>
class PlanCacheEntryBase {
public:
    /**
     * A description of the query from which a 'PlanCacheEntryBase' was created.
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
            return size;
        }

        std::string debugString() const {
            return str::stream() << "query: " << filter.toString() << "; sort: " << sort.toString()
                                 << "; projection: " << projection.toString()
                                 << "; collation: " << collation.toString();
        }

        BSONObj filter;
        BSONObj sort;
        BSONObj projection;
        BSONObj collation;
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

        CreatedFromQuery createdFromQuery;

        // Information that went into picking the winning plan and also why the other plans lost.
        // Never nullptr.
        std::unique_ptr<const plan_ranker::PlanRankingDecision> decision;
    };

    /**
     * Create a new PlanCacheEntrBase.
     * Grabs any planner-specific data required from the solutions.
     */
    static std::unique_ptr<PlanCacheEntryBase<CachedPlanType>> create(
        const std::vector<QuerySolution*>& solutions,
        std::unique_ptr<const plan_ranker::PlanRankingDecision> decision,
        const CanonicalQuery& query,
        std::unique_ptr<CachedPlanType> cachedPlan,
        uint32_t queryHash,
        uint32_t planCacheKey,
        Date_t timeOfCreation,
        bool isActive,
        size_t works) {
        invariant(decision);

        // If the cumulative size of the plan caches is estimated to remain within a predefined
        // threshold, then then include additional debug info which is not strictly necessary for
        // the plan cache to be functional. Once the cumulative plan cache size exceeds this
        // threshold, omit this debug info as a heuristic to prevent plan cache memory consumption
        // from growing too large.
        const bool includeDebugInfo = planCacheTotalSizeEstimateBytes.get() <
            internalQueryCacheMaxSizeBytesBeforeStripDebugInfo.load();

        boost::optional<DebugInfo> debugInfo;
        if (includeDebugInfo) {
            // Strip projections on $-prefixed fields, as these are added by internal callers of the
            // system and are not considered part of the user projection.
            const FindCommandRequest& findCommand = query.getFindCommandRequest();
            BSONObjBuilder projBuilder;
            for (auto elem : findCommand.getProjection()) {
                if (elem.fieldName()[0] == '$') {
                    continue;
                }
                projBuilder.append(elem);
            }

            CreatedFromQuery createdFromQuery{
                findCommand.getFilter(),
                findCommand.getSort(),
                projBuilder.obj(),
                query.getCollator() ? query.getCollator()->getSpec().toBSON() : BSONObj()};
            debugInfo.emplace(std::move(createdFromQuery), std::move(decision));
        }

        return std::unique_ptr<PlanCacheEntryBase<CachedPlanType>>(
            new PlanCacheEntryBase<CachedPlanType>(std::move(cachedPlan),
                                                   timeOfCreation,
                                                   queryHash,
                                                   planCacheKey,
                                                   isActive,
                                                   works,
                                                   std::move(debugInfo)));
    }

    ~PlanCacheEntryBase() {
        planCacheTotalSizeEstimateBytes.decrement(estimatedEntrySizeBytes);
    }

    /**
     * Make a deep copy.
     */
    std::unique_ptr<PlanCacheEntryBase<CachedPlanType>> clone() const {
        boost::optional<DebugInfo> debugInfoCopy;
        if (debugInfo) {
            debugInfoCopy.emplace(*debugInfo);
        }

        return std::unique_ptr<PlanCacheEntryBase<CachedPlanType>>(
            new PlanCacheEntryBase<CachedPlanType>(cachedPlan->clone(),
                                                   timeOfCreation,
                                                   queryHash,
                                                   planCacheKey,
                                                   isActive,
                                                   works,
                                                   std::move(debugInfoCopy)));
    }

    std::string debugString() const {
        StringBuilder builder;
        builder << "(";
        builder << "queryHash: " << queryHash;
        builder << "; planCacheKey: " << planCacheKey;
        if (debugInfo) {
            builder << "; ";
            builder << debugInfo->createdFromQuery.debugString();
        }
        builder << "; timeOfCreation: " << timeOfCreation.toString() << ")";
        return builder.str();
    }

    // A cached plan that can be used to reconstitute the complete execution plan from cache.
    const std::unique_ptr<CachedPlanType> cachedPlan;

    const Date_t timeOfCreation;

    // Hash of the cache key. Intended as an identifier for the query shape in logs and other
    // diagnostic output.
    const uint32_t queryHash;

    // Hash of the "stable" cache key, which is the same regardless of what indexes are around.
    const uint32_t planCacheKey;

    // Whether or not the cache entry is active. Inactive cache entries should not be used for
    // planning.
    bool isActive = false;

    // The number of "works" required for a plan to run on this shape before it becomes
    // active. This value is also used to determine the number of works necessary in order to
    // trigger a replan. Running a query of the same shape while this cache entry is inactive may
    // cause this value to be increased.
    size_t works = 0;

    // Optional debug info containing detailed statistics. Includes a description of the query which
    // resulted in this plan cache's creation as well as runtime stats from the multi-planner trial
    // period that resulted in this cache entry.
    //
    // Once the estimated cumulative size of the mongod's plan caches exceeds a threshold, this
    // debug info is omitted from new plan cache entries.
    const boost::optional<DebugInfo> debugInfo;

    // An estimate of the size in bytes of this plan cache entry. This is the "deep size",
    // calculated by recursively incorporating the size of owned objects, the objects that they in
    // turn own, and so on.
    const uint64_t estimatedEntrySizeBytes;

    /**
     * Tracks the approximate cumulative size of the plan cache entries across all the collections.
     */
    inline static Counter64 planCacheTotalSizeEstimateBytes;

private:
    /**
     * All arguments constructor.
     */
    PlanCacheEntryBase(std::unique_ptr<CachedPlanType> cachedPlan,
                       Date_t timeOfCreation,
                       uint32_t queryHash,
                       uint32_t planCacheKey,
                       bool isActive,
                       size_t works,
                       boost::optional<DebugInfo> debugInfo)
        : cachedPlan(std::move(cachedPlan)),
          timeOfCreation(timeOfCreation),
          queryHash(queryHash),
          planCacheKey(planCacheKey),
          isActive(isActive),
          works(works),
          debugInfo(std::move(debugInfo)),
          estimatedEntrySizeBytes(_estimateObjectSizeInBytes()) {
        invariant(this->cachedPlan);
        // Account for the object in the global metric for estimating the server's total plan cache
        // memory consumption.
        planCacheTotalSizeEstimateBytes.increment(estimatedEntrySizeBytes);
    }

    // Ensure that PlanCacheEntryBase is non-copyable.
    PlanCacheEntryBase(const PlanCacheEntryBase&) = delete;
    PlanCacheEntryBase& operator=(const PlanCacheEntryBase&) = delete;

    uint64_t _estimateObjectSizeInBytes() const {
        uint64_t size = sizeof(PlanCacheEntryBase);
        size += cachedPlan->estimateObjectSizeInBytes();

        if (debugInfo) {
            size += debugInfo->estimateObjectSizeInBytes();
        }

        return size;
    }
};

using PlanCacheEntry = PlanCacheEntryBase<SolutionCacheData>;

/**
 * Caches the best solution to a query.  Aside from the (CanonicalQuery -> QuerySolution)
 * mapping, the cache contains information on why that mapping was made and statistics on the
 * cache entry's actual performance on subsequent runs.
 */
template <class KeyType, class CachedPlanType, class KeyHasher = std::hash<KeyType>>
class PlanCacheBase {
private:
    PlanCacheBase(const PlanCacheBase&) = delete;
    PlanCacheBase& operator=(const PlanCacheBase&) = delete;

public:
    // We have three states for a cache entry to be in. Rather than just 'present' or 'not
    // present', we use a notion of 'inactive entries' as a way of remembering how performant our
    // original solution to the query was. This information is useful to prevent much slower
    // queries from putting their plans in the cache immediately, which could cause faster queries
    // to run with a sub-optimal plan. Since cache entries must go through the "vetting" process of
    // being inactive, we protect ourselves from the possibility of simply adding a cache entry
    // with a very high works value which will never be evicted.
    enum CacheEntryState {
        // There is no cache entry for the given query shape.
        kNotPresent,

        // There is a cache entry for the given query shape, but it is inactive, meaning that it
        // should not be used when planning.
        kPresentInactive,

        // There is a cache entry for the given query shape, and it is active.
        kPresentActive,
    };

    /**
     * Encapsulates the value returned from a call to get().
     */
    struct GetResult {
        CacheEntryState state;
        std::unique_ptr<CachedPlanHolder<CachedPlanType>> cachedPlanHolder;
    };

    /**
     * We don't want to cache every possible query. This function
     * encapsulates the criteria for what makes a canonical query
     * suitable for lookup/inclusion in the cache.
     */
    static bool shouldCacheQuery(const CanonicalQuery& query) {
        const FindCommandRequest& findCommand = query.getFindCommandRequest();
        const MatchExpression* expr = query.root();

        // Collection scan
        // No sort order requested
        if (!query.getSortPattern() && expr->matchType() == MatchExpression::AND &&
            expr->numChildren() == 0) {
            return false;
        }

        // Hint provided
        if (!findCommand.getHint().isEmpty()) {
            return false;
        }

        // Min provided
        // Min queries are a special case of hinted queries.
        if (!findCommand.getMin().isEmpty()) {
            return false;
        }

        // Max provided
        // Similar to min, max queries are a special case of hinted queries.
        if (!findCommand.getMax().isEmpty()) {
            return false;
        }

        // We don't read or write from the plan cache for explain. This ensures
        // that explain queries don't affect cache state, and it also makes
        // sure that we can always generate information regarding rejected plans
        // and/or trial period execution of candidate plans.
        if (query.getExplain()) {
            return false;
        }

        // Tailable cursors won't get cached, just turn into collscans.
        if (query.getFindCommandRequest().getTailable()) {
            return false;
        }

        return true;
    }

    /**
     * If omitted, namespace set to empty string.
     */
    PlanCacheBase() : PlanCacheBase(internalQueryCacheMaxEntriesPerCollection.load()) {}

    PlanCacheBase(size_t size) : _cache(size) {}

    ~PlanCacheBase() = default;

    /**
     * Record solutions for query. Best plan is first element in list.
     * Each query in the cache will have more than 1 plan because we only
     * add queries which are considered by the multi plan runner (which happens
     * only when the query planner generates multiple candidate plans). Callers are responsible
     * for passing the current time so that the time the plan cache entry was created is stored
     * in the plan cache.
     *
     * 'worksGrowthCoefficient' specifies what multiplier to use when growing the 'works' value of
     * an inactive cache entry.  If boost::none is provided, the function will use
     * 'internalQueryCacheWorksGrowthCoefficient'.
     *
     * If the mapping was set successfully, returns Status::OK(), even if it evicted another entry.
     */
    Status set(const CanonicalQuery& query,
               std::unique_ptr<CachedPlanType> cachedPlan,
               const std::vector<QuerySolution*>& solns,
               std::unique_ptr<plan_ranker::PlanRankingDecision> why,
               Date_t now,
               boost::optional<double> worksGrowthCoefficient = boost::none) {
        invariant(why);
        invariant(cachedPlan);

        if (solns.empty()) {
            return Status(ErrorCodes::BadValue, "no solutions provided");
        }

        auto statsSize =
            stdx::visit([](auto&& stats) { return stats.candidatePlanStats.size(); }, why->stats);
        if (statsSize != solns.size()) {
            return Status(ErrorCodes::BadValue, "number of stats in decision must match solutions");
        }

        if (why->scores.size() != why->candidateOrder.size()) {
            return Status(ErrorCodes::BadValue,
                          "number of scores in decision must match viable candidates");
        }

        if (why->candidateOrder.size() + why->failedCandidates.size() != solns.size()) {
            return Status(
                ErrorCodes::BadValue,
                "the number of viable candidates plus the number of failed candidates must "
                "match the number of solutions");
        }

        auto newWorks = stdx::visit(
            visit_helper::Overloaded{[](const plan_ranker::StatsDetails& details) {
                                         return details.candidatePlanStats[0]->common.works;
                                     },
                                     [](const plan_ranker::SBEStatsDetails& details) {
                                         return calculateNumberOfReads(
                                             details.candidatePlanStats[0].get());
                                     }},
            why->stats);
        const auto key = computeKey(query);
        stdx::lock_guard<Latch> cacheLock(_cacheMutex);
        bool isNewEntryActive = false;
        uint32_t queryHash;
        uint32_t planCacheKey;
        if (internalQueryCacheDisableInactiveEntries.load()) {
            // All entries are always active.
            isNewEntryActive = true;
            planCacheKey = key.planCacheKeyHash();
            queryHash = key.queryHash();
        } else {
            PlanCacheEntryBase<CachedPlanType>* oldEntry = nullptr;
            Status cacheStatus = _cache.get(key, &oldEntry);
            invariant(cacheStatus.isOK() || cacheStatus == ErrorCodes::NoSuchKey);
            if (oldEntry) {
                queryHash = oldEntry->queryHash;
                planCacheKey = oldEntry->planCacheKey;
            } else {
                planCacheKey = key.planCacheKeyHash();
                queryHash = key.queryHash();
            }

            const auto newState = getNewEntryState(
                query,
                queryHash,
                planCacheKey,
                oldEntry,
                newWorks,
                worksGrowthCoefficient.get_value_or(internalQueryCacheWorksGrowthCoefficient));

            if (!newState.shouldBeCreated) {
                return Status::OK();
            }
            isNewEntryActive = newState.shouldBeActive;
        }

        auto newEntry(PlanCacheEntryBase<CachedPlanType>::create(solns,
                                                                 std::move(why),
                                                                 query,
                                                                 std::move(cachedPlan),
                                                                 queryHash,
                                                                 planCacheKey,
                                                                 now,
                                                                 isNewEntryActive,
                                                                 newWorks));

        auto evictedEntry = _cache.add(key, newEntry.release());

        if (nullptr != evictedEntry.get()) {
            log_detail::logCacheEviction(query.nss(), evictedEntry->debugString());
        }

        return Status::OK();
    }

    /**
     * Set a cache entry back to the 'inactive' state. Rather than completely evicting an entry
     * when the associated plan starts to perform poorly, we deactivate it, so that plans which
     * perform even worse than the one already in the cache may not easily take its place.
     */
    void deactivate(const CanonicalQuery& query) {
        if (internalQueryCacheDisableInactiveEntries.load()) {
            // This is a noop if inactive entries are disabled.
            return;
        }

        KeyType key = computeKey(query);
        stdx::lock_guard<Latch> cacheLock(_cacheMutex);
        PlanCacheEntryBase<CachedPlanType>* entry = nullptr;
        Status cacheStatus = _cache.get(key, &entry);
        if (!cacheStatus.isOK()) {
            invariant(cacheStatus == ErrorCodes::NoSuchKey);
            return;
        }
        invariant(entry);
        entry->isActive = false;
    }

    /**
     * Look up the cached data access for the provided 'query'.  Used by the query planner
     * to shortcut planning.
     *
     * The return value will provide the "state" of the cache entry, as well as the CachedSolution
     * for the query (if there is one).
     */
    GetResult get(const CanonicalQuery& query) const {
        KeyType key = computeKey(query);
        return get(key);
    }

    /**
     * Look up the cached data access for the provided key. Circumvents the recalculation
     * of a plan cache key.
     *
     * The return value will provide the "state" of the cache entry, as well as the CachedSolution
     * for the query (if there is one).
     */
    GetResult get(const KeyType& key) const {
        stdx::lock_guard<Latch> cacheLock(_cacheMutex);
        PlanCacheEntryBase<CachedPlanType>* entry = nullptr;
        Status cacheStatus = _cache.get(key, &entry);
        if (!cacheStatus.isOK()) {
            invariant(cacheStatus == ErrorCodes::NoSuchKey);
            return {CacheEntryState::kNotPresent, nullptr};
        }
        invariant(entry);

        auto state =
            entry->isActive ? CacheEntryState::kPresentActive : CacheEntryState::kPresentInactive;
        return {state, std::make_unique<CachedPlanHolder<CachedPlanType>>(*entry)};
    }

    /**
     * If the cache entry exists and is active, return a CachedSolution. If the cache entry is
     * inactive, log a message and return a nullptr. If no cache entry exists, return a nullptr.
     */
    std::unique_ptr<CachedPlanHolder<CachedPlanType>> getCacheEntryIfActive(
        const KeyType& key) const {
        auto res = get(key);
        if (res.state == CacheEntryState::kPresentInactive) {
            log_detail::logInactiveCacheEntry(key.toString());
            return nullptr;
        }

        return std::move(res.cachedPlanHolder);
    }

    /**
     * Remove the entry corresponding to 'cq' from the cache.  Returns Status::OK() if the plan
     * was present and removed and an error status otherwise.
     */
    Status remove(const CanonicalQuery& cq) {
        stdx::lock_guard<Latch> cacheLock(_cacheMutex);
        return _cache.remove(computeKey(cq));
    }

    /**
     * Remove *all* cached plans.  Does not clear index information.
     */
    void clear() {
        stdx::lock_guard<Latch> cacheLock(_cacheMutex);
        _cache.clear();
    }

    /**
     * Get the cache key corresponding to the given canonical query.  The query need not already
     * be cached.
     *
     * This is provided in the public API simply as a convenience for consumers who need some
     * description of query shape (e.g. index filters).
     *
     * Callers must hold the collection lock when calling this method.
     */
    KeyType computeKey(const CanonicalQuery& cq) const;

    /**
     * Returns a copy of a cache entry, looked up by CanonicalQuery.
     *
     * If there is no entry in the cache for the 'query', returns an error Status.
     */
    StatusWith<std::unique_ptr<PlanCacheEntryBase<CachedPlanType>>> getEntry(
        const CanonicalQuery& cq) const {
        KeyType key = computeKey(cq);

        stdx::lock_guard<Latch> cacheLock(_cacheMutex);
        PlanCacheEntryBase<CachedPlanType>* entry;
        Status cacheStatus = _cache.get(key, &entry);
        if (!cacheStatus.isOK()) {
            return cacheStatus;
        }
        invariant(entry);

        return std::unique_ptr<PlanCacheEntryBase<CachedPlanType>>(entry->clone());
    }

    /**
     * Returns a vector of all cache entries.
     * Used by planCacheListQueryShapes and index_filter_commands_test.cpp.
     */
    std::vector<std::unique_ptr<PlanCacheEntryBase<CachedPlanType>>> getAllEntries() const {
        stdx::lock_guard<Latch> cacheLock(_cacheMutex);
        std::vector<std::unique_ptr<PlanCacheEntryBase<CachedPlanType>>> entries;

        for (auto&& cacheEntry : _cache) {
            auto entry = cacheEntry.second;
            entries.push_back(std::unique_ptr<PlanCacheEntryBase<CachedPlanType>>(entry->clone()));
        }

        return entries;
    }

    /**
     * Returns number of entries in cache. Includes inactive entries.
     * Used for testing.
     */
    size_t size() const {
        stdx::lock_guard<Latch> cacheLock(_cacheMutex);
        return _cache.size();
    }

    /**
     * Updates internal state kept about the collection's indexes.  Must be called when the set
     * of indexes on the associated collection have changed.
     *
     * Callers must hold the collection lock in exclusive mode when calling this method.
     */
    void notifyOfIndexUpdates(const std::vector<CoreIndexInfo>& indexCores) {
        _indexabilityState.updateDiscriminators(indexCores);
    }

    /**
     * Iterates over the plan cache. For each entry, serializes the PlanCacheEntryBase according to
     * 'serializationFunc'. Returns a vector of all serialized entries which match 'filterFunc'.
     */
    std::vector<BSONObj> getMatchingStats(
        const std::function<BSONObj(const PlanCacheEntryBase<CachedPlanType>&)>& serializationFunc,
        const std::function<bool(const BSONObj&)>& filterFunc) const {
        std::vector<BSONObj> results;
        stdx::lock_guard<Latch> cacheLock(_cacheMutex);

        for (auto&& cacheEntry : _cache) {
            const auto entry = cacheEntry.second;
            auto serializedEntry = serializationFunc(*entry);
            if (filterFunc(serializedEntry)) {
                results.push_back(serializedEntry);
            }
        }

        return results;
    }

private:
    struct NewEntryState {
        bool shouldBeCreated = false;
        bool shouldBeActive = false;
    };

    /**
     * Given a query, and an (optional) current cache entry for its shape ('oldEntry'), determine
     * whether:
     * - We should create a new entry
     * - The new entry should be marked 'active'
     */
    NewEntryState getNewEntryState(const CanonicalQuery& query,
                                   uint32_t queryHash,
                                   uint32_t planCacheKey,
                                   PlanCacheEntryBase<CachedPlanType>* oldEntry,
                                   size_t newWorks,
                                   double growthCoefficient) {
        NewEntryState res;
        if (!oldEntry) {
            log_detail::logCreateInactiveCacheEntry(query.toStringShort(),
                                                    zeroPaddedHex(queryHash),
                                                    zeroPaddedHex(planCacheKey),
                                                    newWorks);
            res.shouldBeCreated = true;
            res.shouldBeActive = false;
            return res;
        }

        if (oldEntry->isActive && newWorks <= oldEntry->works) {
            // The new plan did better than the currently stored active plan. This case may
            // occur if many MultiPlanners are run simultaneously.
            log_detail::logReplaceActiveCacheEntry(query.toStringShort(),
                                                   zeroPaddedHex(queryHash),
                                                   zeroPaddedHex(planCacheKey),
                                                   oldEntry->works,
                                                   newWorks);
            res.shouldBeCreated = true;
            res.shouldBeActive = true;
        } else if (oldEntry->isActive) {
            log_detail::logNoop(query.toStringShort(),
                                zeroPaddedHex(queryHash),
                                zeroPaddedHex(planCacheKey),
                                oldEntry->works,
                                newWorks);
            // There is already an active cache entry with a lower works value.
            // We do nothing.
            res.shouldBeCreated = false;
        } else if (newWorks > oldEntry->works) {
            // This plan performed worse than expected. Rather than immediately overwriting the
            // cache, lower the bar to what is considered good performance and keep the entry
            // inactive.

            // Be sure that 'works' always grows by at least 1, in case its current
            // value and 'internalQueryCacheWorksGrowthCoefficient' are low enough that
            // the old works * new works cast to size_t is the same as the previous value of
            // 'works'.
            const double increasedWorks = std::max(
                oldEntry->works + 1u, static_cast<size_t>(oldEntry->works * growthCoefficient));

            log_detail::logIncreasingWorkValue(query.toStringShort(),
                                               zeroPaddedHex(queryHash),
                                               zeroPaddedHex(planCacheKey),
                                               oldEntry->works,
                                               increasedWorks);
            oldEntry->works = increasedWorks;

            // Don't create a new entry.
            res.shouldBeCreated = false;
        } else {
            // This plan performed just as well or better than we expected, based on the
            // inactive entry's works. We use this as an indicator that it's safe to
            // cache (as an active entry) the plan this query used for the future.
            log_detail::logPromoteCacheEntry(query.toStringShort(),
                                             zeroPaddedHex(queryHash),
                                             zeroPaddedHex(planCacheKey),
                                             oldEntry->works,
                                             newWorks);
            // We'll replace the old inactive entry with an active entry.
            res.shouldBeCreated = true;
            res.shouldBeActive = true;
        }

        return res;
    }

    LRUKeyValue<KeyType, PlanCacheEntryBase<CachedPlanType>, KeyHasher> _cache;

    // Protects _cache.
    mutable Mutex _cacheMutex = MONGO_MAKE_LATCH("PlanCache::_cacheMutex");

    // Holds computed information about the collection's indexes.  Used for generating plan
    // cache keys.
    //
    // Concurrent access is synchronized by the collection lock.  Multiple concurrent readers
    // are allowed.
    PlanCacheIndexabilityState _indexabilityState;
};

using PlanCache = PlanCacheBase<PlanCacheKey, SolutionCacheData, PlanCacheKeyHasher>;
}  // namespace mongo
