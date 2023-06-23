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


#include "mongo/db/query/plan_cache_callbacks.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo::log_detail {
void logInactiveCacheEntry(const std::string& key) {
    LOGV2_DEBUG(
        20936, 2, "Not using cached entry since it is inactive", "cacheKey"_attr = redact(key));
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

void logRemoveCacheEntry(const std::string& ns,
                         const BSONObj& query,
                         const BSONObj& projection,
                         const BSONObj& sort,
                         const BSONObj& collation) {
    LOGV2_DEBUG(23907,
                1,
                "{namespace}: Removed plan cache entry - {query}"
                "(sort: {sort}; projection: {projection}; collation: {collation})",
                "Removed plan cache entry",
                "namespace"_attr = ns,
                "query"_attr = redact(query),
                "sort"_attr = sort,
                "projection"_attr = projection,
                "collation"_attr = collation);
}

void logMissingCacheEntry(const std::string& ns,
                          const BSONObj& query,
                          const BSONObj& projection,
                          const BSONObj& sort,
                          const BSONObj& collation) {
    LOGV2_DEBUG(23906,
                1,
                "{namespace}: Query shape doesn't exist in PlanCache - {query}"
                "(sort: {sort}; projection: {projection}; collation: {collation})",
                "Query shape doesn't exist in PlanCache",
                "namespace"_attr = ns,
                "query"_attr = redact(query),
                "sort"_attr = sort,
                "projection"_attr = projection,
                "collation"_attr = collation);
}

}  // namespace mongo::log_detail
