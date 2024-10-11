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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/query/plan_cache.h"

#include <boost/iterator/transform_iterator.hpp>

#include <algorithm>
#include <math.h>
#include <memory>
#include <vector>

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/base/string_data_comparator_interface.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/query/canonical_query_encoder.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/plan_ranker.h"
#include "mongo/db/query/planner_ixselect.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/hex.h"
#include "mongo/util/transitional_tools_do_not_use/vector_spooling.h"
#include "mongo/util/visit_helper.h"

namespace mongo {
namespace {

ServerStatusMetricField<Counter64> totalPlanCacheSizeEstimateBytesMetric(
    "query.planCacheTotalSizeEstimateBytes", &PlanCacheEntry::planCacheTotalSizeEstimateBytes);

void encodeIndexabilityForDiscriminators(const MatchExpression* tree,
                                         const IndexToDiscriminatorMap& discriminators,
                                         StringBuilder* keyBuilder) {
    for (auto&& indexAndDiscriminatorPair : discriminators) {
        *keyBuilder << indexAndDiscriminatorPair.second.isMatchCompatibleWithIndex(tree);
    }
}

void encodeIndexabilityRecursive(const MatchExpression* tree,
                                 const PlanCacheIndexabilityState& indexabilityState,
                                 StringBuilder* keyBuilder) {
    if (!tree->path().empty()) {
        const IndexToDiscriminatorMap& discriminators =
            indexabilityState.getPathDiscriminators(tree->path());
        IndexToDiscriminatorMap wildcardDiscriminators =
            indexabilityState.buildWildcardDiscriminators(tree->path());
        if (!discriminators.empty() || !wildcardDiscriminators.empty()) {
            *keyBuilder << kEncodeDiscriminatorsBegin;
            // For each discriminator on this path, append the character '0' or '1'.
            encodeIndexabilityForDiscriminators(tree, discriminators, keyBuilder);
            encodeIndexabilityForDiscriminators(tree, wildcardDiscriminators, keyBuilder);

            *keyBuilder << kEncodeDiscriminatorsEnd;
        }
    } else if (tree->matchType() == MatchExpression::MatchType::NOT) {
        // If the node is not compatible with any type of index, add a single '0' discriminator
        // here. Otherwise add a '1'.
        *keyBuilder << kEncodeDiscriminatorsBegin;
        *keyBuilder << QueryPlannerIXSelect::logicalNodeMayBeSupportedByAnIndex(tree);
        *keyBuilder << kEncodeDiscriminatorsEnd;
    }

    for (size_t i = 0; i < tree->numChildren(); ++i) {
        encodeIndexabilityRecursive(tree->getChild(i), indexabilityState, keyBuilder);
    }
}

void encodeIndexability(const MatchExpression* tree,
                        const PlanCacheIndexabilityState& indexabilityState,
                        StringBuilder* keyBuilder) {
    // Before encoding the indexability of the leaf MatchExpressions, apply the global
    // discriminators to the expression as a whole. This is for cases such as partial indexes which
    // must discriminate based on the entire query.
    const auto& globalDiscriminators = indexabilityState.getGlobalDiscriminators();
    if (!globalDiscriminators.empty()) {
        *keyBuilder << kEncodeGlobalDiscriminatorsBegin;
        for (auto&& indexAndDiscriminatorPair : globalDiscriminators) {
            *keyBuilder << indexAndDiscriminatorPair.second.isMatchCompatibleWithIndex(tree);
        }
        *keyBuilder << kEncodeGlobalDiscriminatorsEnd;
    }

    encodeIndexabilityRecursive(tree, indexabilityState, keyBuilder);
}

}  // namespace

std::ostream& operator<<(std::ostream& stream, const PlanCacheKey& key) {
    stream << key.stringData();
    return stream;
}

StringBuilder& operator<<(StringBuilder& builder, const PlanCacheKey& key) {
    builder << key.stringData();
    return builder;
}

//
// Cache-related functions for CanonicalQuery
//

bool PlanCache::shouldCacheQuery(const CanonicalQuery& query) {
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

CachedSolution::CachedSolution(const PlanCacheEntry& entry)
    : plannerData(entry.plannerData->clone()), decisionWorks(entry.works) {}

//
// PlanCacheEntry
//

std::unique_ptr<PlanCacheEntry> PlanCacheEntry::create(
    const std::vector<QuerySolution*>& solutions,
    std::unique_ptr<const plan_ranker::PlanRankingDecision> decision,
    const CanonicalQuery& query,
    uint32_t queryHash,
    uint32_t planCacheKey,
    Date_t timeOfCreation,
    bool isActive,
    size_t works) {
    invariant(decision);

    // The caller of this constructor is responsible for ensuring that the QuerySolution has
    // valid cacheData. If there's no data to cache you shouldn't be trying to construct a
    // PlanCacheEntry.
    //
    // The first solution is the winner, so we can discard the cache data from all subsequent
    // solutions.
    invariant(!solutions.empty());
    invariant(solutions[0]->cacheData);
    auto plannerDataForCache = solutions[0]->cacheData->clone();

    // If the cumulative size of the plan caches is estimated to remain within a predefined
    // threshold, then then include additional debug info which is not strictly necessary for the
    // plan cache to be functional. Once the cumulative plan cache size exceeds this threshold, omit
    // this debug info as a heuristic to prevent plan cache memory consumption from growing too
    // large.
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
            findCommand.getFilter().getOwned(),
            findCommand.getSort().getOwned(),
            projBuilder.obj(),
            query.getCollator() ? query.getCollator()->getSpec().toBSON() : BSONObj()};
        debugInfo.emplace(std::move(createdFromQuery), std::move(decision));
    }

    return std::unique_ptr<PlanCacheEntry>(new PlanCacheEntry(std::move(plannerDataForCache),
                                                              timeOfCreation,
                                                              queryHash,
                                                              planCacheKey,
                                                              isActive,
                                                              works,
                                                              std::move(debugInfo)));
}

PlanCacheEntry::PlanCacheEntry(std::unique_ptr<const SolutionCacheData> plannerData,
                               const Date_t timeOfCreation,
                               const uint32_t queryHash,
                               const uint32_t planCacheKey,
                               const bool isActive,
                               const size_t works,
                               boost::optional<DebugInfo> debugInfo)
    : plannerData(std::move(plannerData)),
      timeOfCreation(timeOfCreation),
      queryHash(queryHash),
      planCacheKey(planCacheKey),
      isActive(isActive),
      works(works),
      queryCounters(0),
      debugInfo(std::move(debugInfo)),
      estimatedEntrySizeBytes(_estimateObjectSizeInBytes()) {
    invariant(this->plannerData);
    // Account for the object in the global metric for estimating the server's total plan cache
    // memory consumption.
    planCacheTotalSizeEstimateBytes.increment(estimatedEntrySizeBytes);
}

PlanCacheEntry::~PlanCacheEntry() {
    planCacheTotalSizeEstimateBytes.decrement(estimatedEntrySizeBytes);
}

std::unique_ptr<PlanCacheEntry> PlanCacheEntry::clone() const {
    boost::optional<DebugInfo> debugInfoCopy;
    if (debugInfo) {
        debugInfoCopy.emplace(*debugInfo);
    }

    return std::unique_ptr<PlanCacheEntry>(new PlanCacheEntry(plannerData->clone(),
                                                              timeOfCreation,
                                                              queryHash,
                                                              planCacheKey,
                                                              isActive,
                                                              works,
                                                              std::move(debugInfoCopy)));
}

uint64_t PlanCacheEntry::CreatedFromQuery::estimateObjectSizeInBytes() const {
    uint64_t size = 0;
    size += filter.objsize();
    size += sort.objsize();
    size += projection.objsize();
    size += collation.objsize();
    return size;
}

PlanCacheEntry::DebugInfo::DebugInfo(
    CreatedFromQuery createdFromQuery,
    std::unique_ptr<const plan_ranker::PlanRankingDecision> decision)
    : createdFromQuery(std::move(createdFromQuery)), decision(std::move(decision)) {
    invariant(this->decision);
}

PlanCacheEntry::DebugInfo::DebugInfo(const DebugInfo& other)
    : createdFromQuery(other.createdFromQuery), decision(other.decision->clone()) {}

PlanCacheEntry::DebugInfo& PlanCacheEntry::DebugInfo::operator=(const DebugInfo& other) {
    createdFromQuery = other.createdFromQuery;
    decision = other.decision->clone();
    return *this;
}

uint64_t PlanCacheEntry::DebugInfo::estimateObjectSizeInBytes() const {
    uint64_t size = 0;
    size += createdFromQuery.estimateObjectSizeInBytes();
    size += decision->estimateObjectSizeInBytes();
    return size;
}

uint64_t PlanCacheEntry::_estimateObjectSizeInBytes() const {
    uint64_t size = sizeof(PlanCacheEntry);
    size += plannerData->estimateObjectSizeInBytes();

    if (debugInfo) {
        size += debugInfo->estimateObjectSizeInBytes();
    }

    return size;
}

std::string PlanCacheEntry::CreatedFromQuery::debugString() const {
    return str::stream() << "query: " << filter.toString() << "; sort: " << sort.toString()
                         << "; projection: " << projection.toString()
                         << "; collation: " << collation.toString();
}

std::string PlanCacheEntry::debugString() const {
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

//
// PlanCacheIndexTree
//

void PlanCacheIndexTree::setIndexEntry(const IndexEntry& ie) {
    entry.reset(new IndexEntry(ie));
}

PlanCacheIndexTree* PlanCacheIndexTree::clone() const {
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

std::string PlanCacheIndexTree::toString(int indents) const {
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

//
// SolutionCacheData
//

std::unique_ptr<SolutionCacheData> SolutionCacheData::clone() const {
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

std::string SolutionCacheData::toString() const {
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

//
// PlanCache
//

PlanCache::PlanCache() : PlanCache(internalQueryCacheMaxEntriesPerCollection.load()) {}

PlanCache::PlanCache(size_t size) : _cache(size) {}

PlanCache::~PlanCache() {}

std::unique_ptr<CachedSolution> PlanCache::getCacheEntryIfActive(const PlanCacheKey& key) const {
    PlanCache::GetResult res = get(key);
    if (res.state == PlanCache::CacheEntryState::kPresentInactive) {
        LOGV2_DEBUG(20936,
                    2,
                    "Not using cached entry since it is inactive",
                    "cacheKey"_attr = redact(key.toString()));
        return nullptr;
    }

    return std::move(res.cachedSolution);
}

/**
 * Given a query, and an (optional) current cache entry for its shape ('oldEntry'), determine
 * whether:
 * - We should create a new entry
 * - The new entry should be marked 'active'
 */
PlanCache::NewEntryState PlanCache::getNewEntryState(const CanonicalQuery& query,
                                                     uint32_t queryHash,
                                                     uint32_t planCacheKey,
                                                     PlanCacheEntry* oldEntry,
                                                     size_t newWorks,
                                                     double growthCoefficient) {
    NewEntryState res;
    if (!oldEntry) {
        LOGV2_DEBUG(20937,
                    1,
                    "Creating inactive cache entry for query",
                    "query"_attr = redact(query.toStringShort()),
                    "queryHash"_attr = zeroPaddedHex(queryHash),
                    "planCacheKey"_attr = zeroPaddedHex(planCacheKey),
                    "newWorks"_attr = newWorks);
        res.shouldBeCreated = true;
        res.shouldBeActive = false;
        return res;
    }

    if (oldEntry->isActive && newWorks <= oldEntry->works) {
        // The new plan did better than the currently stored active plan. This case may
        // occur if many MultiPlanners are run simultaneously.

        LOGV2_DEBUG(20938,
                    1,
                    "Replacing active cache entry for query",
                    "query"_attr = redact(query.toStringShort()),
                    "queryHash"_attr = zeroPaddedHex(queryHash),
                    "planCacheKey"_attr = zeroPaddedHex(planCacheKey),
                    "oldWorks"_attr = oldEntry->works,
                    "newWorks"_attr = newWorks);
        res.shouldBeCreated = true;
        res.shouldBeActive = true;
    } else if (oldEntry->isActive) {
        LOGV2_DEBUG(20939,
                    1,
                    "Attempt to write to the planCache resulted in a noop, since there's already "
                    "an active cache entry with a lower works value",
                    "query"_attr = redact(query.toStringShort()),
                    "queryHash"_attr = zeroPaddedHex(queryHash),
                    "planCacheKey"_attr = zeroPaddedHex(planCacheKey),
                    "newWorks"_attr = newWorks,
                    "oldWorks"_attr = oldEntry->works);
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

        LOGV2_DEBUG(20940,
                    1,
                    "Increasing work value associated with cache entry",
                    "query"_attr = redact(query.toStringShort()),
                    "queryHash"_attr = zeroPaddedHex(queryHash),
                    "planCacheKey"_attr = zeroPaddedHex(planCacheKey),
                    "oldWorks"_attr = oldEntry->works,
                    "increasedWorks"_attr = increasedWorks);
        oldEntry->works = increasedWorks;

        // Don't create a new entry.
        res.shouldBeCreated = false;
    } else {
        // This plan performed just as well or better than we expected, based on the
        // inactive entry's works. We use this as an indicator that it's safe to
        // cache (as an active entry) the plan this query used for the future.
        LOGV2_DEBUG(20941,
                    1,
                    "Inactive cache entry for query is being promoted to active entry",
                    "query"_attr = redact(query.toStringShort()),
                    "queryHash"_attr = zeroPaddedHex(queryHash),
                    "planCacheKey"_attr = zeroPaddedHex(planCacheKey),
                    "oldWorks"_attr = oldEntry->works,
                    "newWorks"_attr = newWorks);
        // We'll replace the old inactive entry with an active entry.
        res.shouldBeCreated = true;
        res.shouldBeActive = true;
    }

    return res;
}


Status PlanCache::set(const CanonicalQuery& query,
                      const std::vector<QuerySolution*>& solns,
                      std::unique_ptr<plan_ranker::PlanRankingDecision> why,
                      Date_t now,
                      boost::optional<double> worksGrowthCoefficient) {
    invariant(why);

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
        return Status(ErrorCodes::BadValue,
                      "the number of viable candidates plus the number of failed candidates must "
                      "match the number of solutions");
    }

    auto newWorks =
        stdx::visit(visit_helper::Overloaded{[](const plan_ranker::StatsDetails& details) {
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
        planCacheKey = canonical_query_encoder::computeHash(key.stringData());
        queryHash = canonical_query_encoder::computeHash(key.getStableKeyStringData());
    } else {
        PlanCacheEntry* oldEntry = nullptr;
        Status cacheStatus = _cache.get(key, &oldEntry);
        invariant(cacheStatus.isOK() || cacheStatus == ErrorCodes::NoSuchKey);
        if (oldEntry) {
            queryHash = oldEntry->queryHash;
            planCacheKey = oldEntry->planCacheKey;
        } else {
            planCacheKey = canonical_query_encoder::computeHash(key.stringData());
            queryHash = canonical_query_encoder::computeHash(key.getStableKeyStringData());
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

    auto newEntry(PlanCacheEntry::create(
        solns, std::move(why), query, queryHash, planCacheKey, now, isNewEntryActive, newWorks));

    std::unique_ptr<PlanCacheEntry> evictedEntry = _cache.add(key, newEntry.release());

    if (nullptr != evictedEntry.get()) {
        LOGV2_DEBUG(20942,
                    1,
                    "Plan cache maximum size exceeded - removed least recently used entry",
                    "namespace"_attr = query.nss(),
                    "evictedEntry"_attr = redact(evictedEntry->debugString()));
    }

    return Status::OK();
}

void PlanCache::deactivate(const CanonicalQuery& query) {
    if (internalQueryCacheDisableInactiveEntries.load()) {
        // This is a noop if inactive entries are disabled.
        return;
    }

    PlanCacheKey key = computeKey(query);
    stdx::lock_guard<Latch> cacheLock(_cacheMutex);
    PlanCacheEntry* entry = nullptr;
    Status cacheStatus = _cache.get(key, &entry);
    if (!cacheStatus.isOK()) {
        invariant(cacheStatus == ErrorCodes::NoSuchKey);
        return;
    }
    invariant(entry);
    entry->isActive = false;
}

//increase the query counters which hit the plan
void PlanCache::increaseCacheQueryCounters(const CanonicalQuery& query) {
    PlanCacheKey key = computeKey(query);
    stdx::lock_guard<Latch> cacheLock(_cacheMutex);
    PlanCacheEntry* entry = nullptr;
    Status cacheStatus = _cache.get(key, &entry);
    if (!cacheStatus.isOK()) {
        invariant(cacheStatus == ErrorCodes::NoSuchKey);
        return;
    }
    invariant(entry);
    entry->queryCounters++;
} 

PlanCache::GetResult PlanCache::get(const CanonicalQuery& query) const {
    PlanCacheKey key = computeKey(query);
    return get(key);
}

PlanCache::GetResult PlanCache::get(const PlanCacheKey& key) const {
    stdx::lock_guard<Latch> cacheLock(_cacheMutex);
    PlanCacheEntry* entry = nullptr;
    Status cacheStatus = _cache.get(key, &entry);
    if (!cacheStatus.isOK()) {
        invariant(cacheStatus == ErrorCodes::NoSuchKey);
        return {CacheEntryState::kNotPresent, nullptr};
    }
    invariant(entry);

    auto state =
        entry->isActive ? CacheEntryState::kPresentActive : CacheEntryState::kPresentInactive;
    return {state, std::make_unique<CachedSolution>(*entry)};
}

Status PlanCache::remove(const CanonicalQuery& canonicalQuery) {
    stdx::lock_guard<Latch> cacheLock(_cacheMutex);
    return _cache.remove(computeKey(canonicalQuery));
}

void PlanCache::clear() {
    stdx::lock_guard<Latch> cacheLock(_cacheMutex);
    _cache.clear();
}

PlanCacheKey PlanCache::computeKey(const CanonicalQuery& cq) const {
    const auto shapeString = cq.encodeKey();

    StringBuilder indexabilityKeyBuilder;
    encodeIndexability(cq.root(), _indexabilityState, &indexabilityKeyBuilder);
    return PlanCacheKey(std::move(shapeString),
                        indexabilityKeyBuilder.str(),
                        cq.getEnableSlotBasedExecutionEngine());
}

StatusWith<std::unique_ptr<PlanCacheEntry>> PlanCache::getEntry(const CanonicalQuery& query) const {
    PlanCacheKey key = computeKey(query);

    stdx::lock_guard<Latch> cacheLock(_cacheMutex);
    PlanCacheEntry* entry;
    Status cacheStatus = _cache.get(key, &entry);
    if (!cacheStatus.isOK()) {
        return cacheStatus;
    }
    invariant(entry);

    return std::unique_ptr<PlanCacheEntry>(entry->clone());
}

std::vector<std::unique_ptr<PlanCacheEntry>> PlanCache::getAllEntries() const {
    stdx::lock_guard<Latch> cacheLock(_cacheMutex);
    std::vector<std::unique_ptr<PlanCacheEntry>> entries;

    for (auto&& cacheEntry : _cache) {
        auto entry = cacheEntry.second;
        entries.push_back(std::unique_ptr<PlanCacheEntry>(entry->clone()));
    }

    return entries;
}

size_t PlanCache::size() const {
    stdx::lock_guard<Latch> cacheLock(_cacheMutex);
    return _cache.size();
}

void PlanCache::notifyOfIndexUpdates(const std::vector<CoreIndexInfo>& indexCores) {
    _indexabilityState.updateDiscriminators(indexCores);
}

std::vector<BSONObj> PlanCache::getMatchingStats(
    const std::function<BSONObj(const PlanCacheEntry&)>& serializationFunc,
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

}  // namespace mongo
