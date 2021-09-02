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

#include "mongo/db/query/plan_cache.h"

#include "mongo/db/query/plan_cache_indexability.h"
#include "mongo/db/query/planner_ixselect.h"
#include "mongo/logv2/log.h"

namespace mongo {
namespace log_detail {
void logInactiveCacheEntry(const std::string& key) {
    LOGV2_DEBUG(
        20936, 2, "Not using cached entry since it is inactive", "cacheKey"_attr = redact(key));
}

void logCacheEviction(NamespaceString nss, std::string&& evictedEntry) {
    LOGV2_DEBUG(20942,
                1,
                "Plan cache maximum size exceeded - removed least recently used entry",
                "namespace"_attr = nss,
                "evictedEntry"_attr = redact(evictedEntry));
}

void logCreateInactiveCacheEntry(std::string&& query,
                                 std::string&& queryHash,
                                 std::string&& planCacheKey,
                                 size_t newWorks) {
    LOGV2_DEBUG(20937,
                1,
                "Creating inactive cache entry for query",
                "query"_attr = redact(query),
                "queryHash"_attr = queryHash,
                "planCacheKey"_attr = planCacheKey,
                "newWorks"_attr = newWorks);
}

void logReplaceActiveCacheEntry(std::string&& query,
                                std::string&& queryHash,
                                std::string&& planCacheKey,
                                size_t works,
                                size_t newWorks) {
    LOGV2_DEBUG(20938,
                1,
                "Replacing active cache entry for query",
                "query"_attr = redact(query),
                "queryHash"_attr = queryHash,
                "planCacheKey"_attr = planCacheKey,
                "oldWorks"_attr = works,
                "newWorks"_attr = newWorks);
}

void logNoop(std::string&& query,
             std::string&& queryHash,
             std::string&& planCacheKey,
             size_t works,
             size_t newWorks) {
    LOGV2_DEBUG(20939,
                1,
                "Attempt to write to the planCache resulted in a noop, since there's already "
                "an active cache entry with a lower works value",
                "query"_attr = redact(query),
                "queryHash"_attr = queryHash,
                "planCacheKey"_attr = planCacheKey,
                "oldWorks"_attr = works,
                "newWorks"_attr = newWorks);
}

void logIncreasingWorkValue(std::string&& query,
                            std::string&& queryHash,
                            std::string&& planCacheKey,
                            size_t works,
                            size_t increasedWorks) {
    LOGV2_DEBUG(20940,
                1,
                "Increasing work value associated with cache entry",
                "query"_attr = redact(query),
                "queryHash"_attr = queryHash,
                "planCacheKey"_attr = planCacheKey,
                "oldWorks"_attr = works,
                "increasedWorks"_attr = increasedWorks);
}

void logPromoteCacheEntry(std::string&& query,
                          std::string&& queryHash,
                          std::string&& planCacheKey,
                          size_t works,
                          size_t newWorks) {
    LOGV2_DEBUG(20941,
                1,
                "Inactive cache entry for query is being promoted to active entry",
                "query"_attr = redact(query),
                "queryHash"_attr = queryHash,
                "planCacheKey"_attr = planCacheKey,
                "oldWorks"_attr = works,
                "newWorks"_attr = newWorks);
}
}  // namespace log_detail

namespace plan_cache_detail {
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
}  // namespace plan_cache_detail
}  // namespace mongo
