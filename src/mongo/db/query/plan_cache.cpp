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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

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
#include "mongo/util/assert_util.h"
#include "mongo/util/hex.h"
#include "mongo/util/log.h"
#include "mongo/util/transitional_tools_do_not_use/vector_spooling.h"

namespace mongo {
namespace {

ServerStatusMetricField<Counter64> totalPlanCacheSizeEstimateBytesMetric(
    "query.planCacheTotalSizeEstimateBytes", &PlanCacheEntry::planCacheTotalSizeEstimateBytes);

// Delimiters for cache key encoding.
const char kEncodeDiscriminatorsBegin = '<';
const char kEncodeDiscriminatorsEnd = '>';

void encodeIndexabilityForDiscriminators(const MatchExpression* tree,
                                         const IndexToDiscriminatorMap& discriminators,
                                         StringBuilder* keyBuilder) {
    for (auto&& indexAndDiscriminatorPair : discriminators) {
        *keyBuilder << indexAndDiscriminatorPair.second.isMatchCompatibleWithIndex(tree);
    }
}

void encodeIndexability(const MatchExpression* tree,
                        const PlanCacheIndexabilityState& indexabilityState,
                        StringBuilder* keyBuilder) {
    if (!tree->path().empty()) {
        const IndexToDiscriminatorMap& discriminators =
            indexabilityState.getDiscriminators(tree->path());
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
        encodeIndexability(tree->getChild(i), indexabilityState, keyBuilder);
    }
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
    const QueryRequest& qr = query.getQueryRequest();
    const MatchExpression* expr = query.root();

    // Collection scan
    // No sort order requested
    if (qr.getSort().isEmpty() && expr->matchType() == MatchExpression::AND &&
        expr->numChildren() == 0) {
        return false;
    }

    // Hint provided
    if (!qr.getHint().isEmpty()) {
        return false;
    }

    // Min provided
    // Min queries are a special case of hinted queries.
    if (!qr.getMin().isEmpty()) {
        return false;
    }

    // Max provided
    // Similar to min, max queries are a special case of hinted queries.
    if (!qr.getMax().isEmpty()) {
        return false;
    }

    // We don't read or write from the plan cache for explain. This ensures
    // that explain queries don't affect cache state, and it also makes
    // sure that we can always generate information regarding rejected plans
    // and/or trial period execution of candidate plans.
    if (qr.isExplain()) {
        return false;
    }

    // Tailable cursors won't get cached, just turn into collscans.
    if (query.getQueryRequest().isTailable()) {
        return false;
    }

    return true;
}

//
// CachedSolution
//

CachedSolution::CachedSolution(const PlanCacheKey& key, const PlanCacheEntry& entry)
    : plannerData(entry.plannerData.size()),
      key(key),
      query(entry.query.getOwned()),
      sort(entry.sort.getOwned()),
      projection(entry.projection.getOwned()),
      collation(entry.collation.getOwned()),
      decisionWorks(entry.works) {
    // CachedSolution should not having any references into
    // cache entry. All relevant data should be cloned/copied.
    for (size_t i = 0; i < entry.plannerData.size(); ++i) {
        verify(entry.plannerData[i]);
        plannerData[i] = entry.plannerData[i]->clone();
    }
}

CachedSolution::~CachedSolution() {
    for (std::vector<SolutionCacheData*>::const_iterator i = plannerData.begin();
         i != plannerData.end();
         ++i) {
        SolutionCacheData* scd = *i;
        delete scd;
    }
}

//
// PlanCacheEntry
//

std::unique_ptr<PlanCacheEntry> PlanCacheEntry::create(
    const std::vector<QuerySolution*>& solutions,
    std::unique_ptr<const PlanRankingDecision> decision,
    const CanonicalQuery& query,
    uint32_t queryHash,
    uint32_t planCacheKey,
    Date_t timeOfCreation,
    bool isActive,
    size_t works) {
    invariant(decision);

    // The caller of this constructor is responsible for ensuring
    // that the QuerySolution 's' has valid cacheData. If there's no
    // data to cache you shouldn't be trying to construct a PlanCacheEntry.

    // Copy the solution's cache data into the plan cache entry.
    std::vector<std::unique_ptr<const SolutionCacheData>> solutionCacheData(solutions.size());
    for (size_t i = 0; i < solutions.size(); ++i) {
        invariant(solutions[i]->cacheData.get());
        solutionCacheData[i] =
            std::unique_ptr<const SolutionCacheData>(solutions[i]->cacheData->clone());
    }

    // Strip projections on $-prefixed fields, as these are added by internal callers of the
    // system and are not considered part of the user projection.
    const QueryRequest& qr = query.getQueryRequest();
    BSONObjBuilder projBuilder;
    for (auto elem : qr.getProj()) {
        if (elem.fieldName()[0] == '$') {
            continue;
        }
        projBuilder.append(elem);
    }

    return std::unique_ptr<PlanCacheEntry>(new PlanCacheEntry(
        std::move(solutionCacheData),
        qr.getFilter(),
        qr.getSort(),
        projBuilder.obj(),
        query.getCollator() ? query.getCollator()->getSpec().toBSON() : BSONObj(),
        timeOfCreation,
        queryHash,
        planCacheKey,
        std::move(decision),
        {},
        isActive,
        works));
}

PlanCacheEntry::PlanCacheEntry(std::vector<std::unique_ptr<const SolutionCacheData>> plannerData,
                               const BSONObj& query,
                               const BSONObj& sort,
                               const BSONObj& projection,
                               const BSONObj& collation,
                               const Date_t timeOfCreation,
                               const uint32_t queryHash,
                               const uint32_t planCacheKey,
                               std::unique_ptr<const PlanRankingDecision> decision,
                               std::vector<double> feedback,
                               const bool isActive,
                               const size_t works)
    : plannerData(std::move(plannerData)),
      query(query),
      sort(sort),
      projection(projection),
      collation(collation),
      timeOfCreation(timeOfCreation),
      queryHash(queryHash),
      planCacheKey(planCacheKey),
      decision(std::move(decision)),
      feedback(std::move(feedback)),
      isActive(isActive),
      works(works),
      _entireObjectSize(_estimateObjectSizeInBytes()) {
    // Account for the object in the global metric for estimating the server's total plan cache
    // memory consumption.
    planCacheTotalSizeEstimateBytes.increment(_entireObjectSize);
}

PlanCacheEntry::~PlanCacheEntry() {
    planCacheTotalSizeEstimateBytes.decrement(_entireObjectSize);
}

std::unique_ptr<PlanCacheEntry> PlanCacheEntry::clone() const {
    std::vector<std::unique_ptr<const SolutionCacheData>> solutionCacheData(plannerData.size());
    for (size_t i = 0; i < plannerData.size(); ++i) {
        invariant(plannerData[i]);
        solutionCacheData[i] = std::unique_ptr<const SolutionCacheData>(plannerData[i]->clone());
    }

    auto decisionPtr = std::unique_ptr<PlanRankingDecision>(decision->clone());
    return std::unique_ptr<PlanCacheEntry>(new PlanCacheEntry(std::move(solutionCacheData),
                                                              query,
                                                              sort,
                                                              projection,
                                                              collation,
                                                              timeOfCreation,
                                                              queryHash,
                                                              planCacheKey,
                                                              std::move(decisionPtr),
                                                              feedback,
                                                              isActive,
                                                              works));
}

uint64_t PlanCacheEntry::_estimateObjectSizeInBytes() const {
    return  // Add the size of each entry in 'plannerData' vector.
        container_size_helper::estimateObjectSizeInBytes(
            plannerData,
            [](const auto& cacheData) { return cacheData->estimateObjectSizeInBytes(); },
            true) +
        // Add size of each entry in '_feedback' vector.
        container_size_helper::estimateObjectSizeInBytes(feedback) +
        // Add the entire size of 'decision' object.
        (decision ? decision->estimateObjectSizeInBytes() : 0) +
        // Add the size of all the owned BSON objects.
        query.objsize() + sort.objsize() + projection.objsize() + collation.objsize() +
        // Add size of the object.
        sizeof(*this);
}

std::string PlanCacheEntry::toString() const {
    return str::stream() << "(query: " << query.toString() << ";sort: " << sort.toString()
                         << ";projection: " << projection.toString()
                         << ";collation: " << collation.toString()
                         << ";solutions: " << plannerData.size()
                         << ";timeOfCreation: " << timeOfCreation.toString() << ")";
}

std::string CachedSolution::toString() const {
    return str::stream() << "key: " << key << '\n';
}

//
// PlanCacheIndexTree
//

void PlanCacheIndexTree::setIndexEntry(const IndexEntry& ie) {
    entry.reset(new IndexEntry(ie));
}

PlanCacheIndexTree* PlanCacheIndexTree::clone() const {
    PlanCacheIndexTree* root = new PlanCacheIndexTree();
    if (NULL != entry.get()) {
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
        if (NULL != entry.get()) {
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

SolutionCacheData* SolutionCacheData::clone() const {
    SolutionCacheData* other = new SolutionCacheData();
    if (NULL != this->tree.get()) {
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

PlanCache::PlanCache() : PlanCache(internalQueryCacheSize.load()) {}

PlanCache::PlanCache(size_t size) : _cache(size) {}

PlanCache::PlanCache(const std::string& ns) : _cache(internalQueryCacheSize.load()), _ns(ns) {}

PlanCache::~PlanCache() {}

std::unique_ptr<CachedSolution> PlanCache::getCacheEntryIfActive(const PlanCacheKey& key) const {

    PlanCache::GetResult res = get(key);
    if (res.state == PlanCache::CacheEntryState::kPresentInactive) {
        LOG(2) << "Not using cached entry for " << redact(res.cachedSolution->toString())
               << " since it is inactive";
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
        LOG(1) << "Creating inactive cache entry for query shape " << redact(query.toStringShort())
               << " queryHash " << unsignedIntToFixedLengthHex(queryHash) << " planCacheKey "
               << unsignedIntToFixedLengthHex(planCacheKey) << " with works value " << newWorks;
        res.shouldBeCreated = true;
        res.shouldBeActive = false;
        return res;
    }

    if (oldEntry->isActive && newWorks <= oldEntry->works) {
        // The new plan did better than the currently stored active plan. This case may
        // occur if many MultiPlanners are run simultaneously.

        LOG(1) << "Replacing active cache entry for query " << redact(query.toStringShort())
               << " queryHash " << unsignedIntToFixedLengthHex(queryHash) << " planCacheKey "
               << unsignedIntToFixedLengthHex(planCacheKey) << " with works " << oldEntry->works
               << " with a plan with works " << newWorks;
        res.shouldBeCreated = true;
        res.shouldBeActive = true;
    } else if (oldEntry->isActive) {
        LOG(1) << "Attempt to write to the planCache for query " << redact(query.toStringShort())
               << " queryHash " << unsignedIntToFixedLengthHex(queryHash) << " planCacheKey "
               << unsignedIntToFixedLengthHex(planCacheKey) << " with a plan with works "
               << newWorks << " is a noop, since there's already a plan with works value "
               << oldEntry->works;
        // There is already an active cache entry with a higher works value.
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

        LOG(1) << "Increasing work value associated with cache entry for query "
               << redact(query.toStringShort()) << " queryHash "
               << unsignedIntToFixedLengthHex(queryHash) << " planCacheKey "
               << unsignedIntToFixedLengthHex(planCacheKey) << " from " << oldEntry->works << " to "
               << increasedWorks;
        oldEntry->works = increasedWorks;

        // Don't create a new entry.
        res.shouldBeCreated = false;
    } else {
        // This plan performed just as well or better than we expected, based on the
        // inactive entry's works. We use this as an indicator that it's safe to
        // cache (as an active entry) the plan this query used for the future.
        LOG(1) << "Inactive cache entry for query " << redact(query.toStringShort())
               << " queryHash " << unsignedIntToFixedLengthHex(queryHash) << " planCacheKey "
               << unsignedIntToFixedLengthHex(planCacheKey) << " with works " << oldEntry->works
               << " is being promoted to active entry with works value " << newWorks;
        // We'll replace the old inactive entry with an active entry.
        res.shouldBeCreated = true;
        res.shouldBeActive = true;
    }

    return res;
}


Status PlanCache::set(const CanonicalQuery& query,
                      const std::vector<QuerySolution*>& solns,
                      std::unique_ptr<PlanRankingDecision> why,
                      Date_t now,
                      boost::optional<double> worksGrowthCoefficient) {
    invariant(why);

    if (solns.empty()) {
        return Status(ErrorCodes::BadValue, "no solutions provided");
    }

    if (why->stats.size() != solns.size()) {
        return Status(ErrorCodes::BadValue, "number of stats in decision must match solutions");
    }

    if (why->scores.size() != solns.size()) {
        return Status(ErrorCodes::BadValue, "number of scores in decision must match solutions");
    }

    if (why->candidateOrder.size() != solns.size()) {
        return Status(ErrorCodes::BadValue,
                      "candidate ordering entries in decision must match solutions");
    }

    const auto key = computeKey(query);
    const size_t newWorks = why->stats[0]->common.works;
    stdx::lock_guard<stdx::mutex> cacheLock(_cacheMutex);
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

    if (NULL != evictedEntry.get()) {
        LOG(1) << _ns << ": plan cache maximum size exceeded - "
               << "removed least recently used entry " << redact(evictedEntry->toString());
    }

    return Status::OK();
}

void PlanCache::deactivate(const CanonicalQuery& query) {
    if (internalQueryCacheDisableInactiveEntries.load()) {
        // This is a noop if inactive entries are disabled.
        return;
    }

    PlanCacheKey key = computeKey(query);
    stdx::lock_guard<stdx::mutex> cacheLock(_cacheMutex);
    PlanCacheEntry* entry = nullptr;
    Status cacheStatus = _cache.get(key, &entry);
    if (!cacheStatus.isOK()) {
        invariant(cacheStatus == ErrorCodes::NoSuchKey);
        return;
    }
    invariant(entry);
    entry->isActive = false;
}

PlanCache::GetResult PlanCache::get(const CanonicalQuery& query) const {
    PlanCacheKey key = computeKey(query);
    return get(key);
}

PlanCache::GetResult PlanCache::get(const PlanCacheKey& key) const {
    stdx::lock_guard<stdx::mutex> cacheLock(_cacheMutex);
    PlanCacheEntry* entry = nullptr;
    Status cacheStatus = _cache.get(key, &entry);
    if (!cacheStatus.isOK()) {
        invariant(cacheStatus == ErrorCodes::NoSuchKey);
        return {CacheEntryState::kNotPresent, nullptr};
    }
    invariant(entry);

    auto state =
        entry->isActive ? CacheEntryState::kPresentActive : CacheEntryState::kPresentInactive;
    return {state, stdx::make_unique<CachedSolution>(key, *entry)};
}

Status PlanCache::feedback(const CanonicalQuery& cq, double score) {
    PlanCacheKey ck = computeKey(cq);

    stdx::lock_guard<stdx::mutex> cacheLock(_cacheMutex);
    PlanCacheEntry* entry;
    Status cacheStatus = _cache.get(ck, &entry);
    if (!cacheStatus.isOK()) {
        return cacheStatus;
    }
    invariant(entry);

    // We store up to a constant number of feedback entries.
    if (entry->feedback.size() < static_cast<size_t>(internalQueryCacheFeedbacksStored.load())) {
        entry->feedback.push_back(score);
    }

    return Status::OK();
}

Status PlanCache::remove(const CanonicalQuery& canonicalQuery) {
    stdx::lock_guard<stdx::mutex> cacheLock(_cacheMutex);
    return _cache.remove(computeKey(canonicalQuery));
}

void PlanCache::clear() {
    stdx::lock_guard<stdx::mutex> cacheLock(_cacheMutex);
    _cache.clear();
}

PlanCacheKey PlanCache::computeKey(const CanonicalQuery& cq) const {
    const auto shapeString = cq.encodeKey();

    StringBuilder indexabilityKeyBuilder;
    encodeIndexability(cq.root(), _indexabilityState, &indexabilityKeyBuilder);
    return PlanCacheKey(std::move(shapeString), indexabilityKeyBuilder.str());
}

StatusWith<std::unique_ptr<PlanCacheEntry>> PlanCache::getEntry(const CanonicalQuery& query) const {
    PlanCacheKey key = computeKey(query);

    stdx::lock_guard<stdx::mutex> cacheLock(_cacheMutex);
    PlanCacheEntry* entry;
    Status cacheStatus = _cache.get(key, &entry);
    if (!cacheStatus.isOK()) {
        return cacheStatus;
    }
    invariant(entry);

    return std::unique_ptr<PlanCacheEntry>(entry->clone());
}

std::vector<std::unique_ptr<PlanCacheEntry>> PlanCache::getAllEntries() const {
    stdx::lock_guard<stdx::mutex> cacheLock(_cacheMutex);
    std::vector<std::unique_ptr<PlanCacheEntry>> entries;

    for (auto&& cacheEntry : _cache) {
        auto entry = cacheEntry.second;
        entries.push_back(std::unique_ptr<PlanCacheEntry>(entry->clone()));
    }

    return entries;
}

size_t PlanCache::size() const {
    stdx::lock_guard<stdx::mutex> cacheLock(_cacheMutex);
    return _cache.size();
}

void PlanCache::notifyOfIndexUpdates(const std::vector<CoreIndexInfo>& indexCores) {
    _indexabilityState.updateDiscriminators(indexCores);
}

std::vector<BSONObj> PlanCache::getMatchingStats(
    const std::function<BSONObj(const PlanCacheEntry&)>& serializationFunc,
    const std::function<bool(const BSONObj&)>& filterFunc) const {
    std::vector<BSONObj> results;
    stdx::lock_guard<stdx::mutex> cacheLock(_cacheMutex);

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
