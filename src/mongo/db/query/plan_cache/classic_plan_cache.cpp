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

#include "mongo/db/query/plan_cache/classic_plan_cache.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/str.h"


namespace mongo {
Counter64& planCacheTotalSizeEstimateBytes =
    *MetricBuilder<Counter64>{"query.planCache.totalSizeEstimateBytes"};
Counter64& planCacheEntries = *MetricBuilder<Counter64>{"query.planCache.totalQueryShapes"};

std::ostream& operator<<(std::ostream& stream, const PlanCacheKey& key) {
    stream << key.toString();
    return stream;
}

void PlanCacheIndexTree::setIndexEntry(const IndexEntry& ie) {
    entry = std::make_unique<IndexEntry>(ie);
}

std::unique_ptr<PlanCacheIndexTree> PlanCacheIndexTree::clone() const {
    auto root = std::make_unique<PlanCacheIndexTree>();
    if (nullptr != entry.get()) {
        root->index_pos = index_pos;
        root->setIndexEntry(*entry.get());
        root->canCombineBounds = canCombineBounds;
    }
    root->orPushdowns = orPushdowns;

    for (auto it = children.begin(); it != children.end(); ++it) {
        root->children.push_back((*it)->clone());
    }
    return root;
}

std::string PlanCacheIndexTree::toString(int indents) const {
    StringBuilder result;
    if (!children.empty()) {
        result << std::string(3 * indents, '-') << "Node\n";
        int newIndent = indents + 1;
        for (auto it = children.begin(); it != children.end(); ++it) {
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

std::unique_ptr<SolutionCacheData> SolutionCacheData::clone() const {
    auto other = std::make_unique<SolutionCacheData>();
    if (nullptr != this->tree.get()) {
        // 'tree' could be NULL if the cached solution is a collection scan.
        other->tree = this->tree->clone();
    }
    if (this->virtualScanData) {
        other->virtualScanData = std::make_unique<VirtualScanCacheData>(*this->virtualScanData);
    }
    other->solnType = this->solnType;
    other->wholeIXSolnDir = this->wholeIXSolnDir;
    other->indexFilterApplied = this->indexFilterApplied;
    other->solutionHash = this->solutionHash;
    return other;
}

std::string SolutionCacheData::toString() const {
    switch (this->solnType) {
        case WHOLE_IXSCAN_SOLN:
            MONGO_verify(this->tree.get());
            return str::stream() << "(whole index scan solution: " << "dir=" << this->wholeIXSolnDir
                                 << "; " << "tree=" << this->tree->toString() << ")";
        case COLLSCAN_SOLN:
            return "(collection scan)";
        case USE_INDEX_TAGS_SOLN:
            MONGO_verify(this->tree.get());
            return str::stream() << "(index-tagged expression tree: " << "tree="
                                 << this->tree->toString() << ")";
        case VIRTSCAN_SOLN:
            return "(virtual scan)";
    }
    MONGO_UNREACHABLE;
}

bool shouldCacheQuery(const CanonicalQuery& query) {
    if (internalQueryDisablePlanCache.load()) {
        return false;
    }

    const FindCommandRequest& findCommand = query.getFindCommandRequest();
    const MatchExpression* expr = query.getPrimaryMatchExpression();

    if (expr->isTriviallyFalse()) {
        return false;
    }

    bool noSortPattern = !query.getSortPattern();
    // For some DISTINCT_SCAN-eligible queries (e.g. $group with $top/$sortBy), the sort pattern is
    // not stored in CanonicalQuery's _sortPattern field. In these cases, we use CanonicalDistinct's
    // _sortRequirement field.
    if (query.getExpCtx()->isFeatureFlagShardFilteringDistinctScanEnabled()) {
        noSortPattern =
            noSortPattern && !(query.getDistinct() && query.getDistinct()->getSortRequirement());
    }

    if (noSortPattern && expr->matchType() == MatchExpression::AND && expr->numChildren() == 0 &&
        !query.isSbeCompatible()) {
        return false;
    }

    // The classic plan cache doesn't have the plan itself, but only some data to re-construct the
    // plan. It is only useful for skipping multiplanning, and hinted queries are generally not
    // multi-planned, so it is not necessary to cache the plan. In contrast, the SBE plan cache has
    // the plan itself, so caching hinted queries could help to skip the plan construction. The
    // SBE plan cache is only on when featureFlagSbeFull is enabled, so in cases where the query
    // is SBE ineligble or the SBE plan cache is disabled, we do not cache at all.
    if (!findCommand.getHint().isEmpty()) {
        if (!query.isSbeCompatible() || !feature_flags::gFeatureFlagSbeFull.isEnabled()) {
            return false;
        }
    }

    if (!findCommand.getMin().isEmpty()) {
        return false;
    }

    if (!findCommand.getMax().isEmpty()) {
        return false;
    }

    // We don't read or write from the plan cache for explain. This ensures that explain queries
    // don't affect cache state, and it also makes sure that we can always generate information
    // regarding rejected plans and/or trial period execution of candidate plans.
    //
    // There is one exception: $lookup's implementation in the DocumentSource engine relies on
    // caching the plan on the inner side in order to avoid repeating the planning process for every
    // document on the outer side. To ensure that the 'executionTime' value is accurate for $lookup,
    // we allow the inner side to use the cache even if the query is an explain.
    tassert(6497600, "expCtx is null", query.getExpCtxRaw());
    if (query.isExplainAndCacheIneligible()) {
        return false;
    }

    // Tailable cursors won't get cached, just turn into collscans.
    if (query.getFindCommandRequest().getTailable()) {
        return false;
    }

    return true;
}
}  // namespace mongo
