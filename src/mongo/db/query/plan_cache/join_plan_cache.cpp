/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/query/plan_cache/join_plan_cache.h"

#include "mongo/logv2/log.h"

#include <mutex>
#include <shared_mutex>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace {

const ServiceContext::Decoration<std::unique_ptr<JoinPlanCache>> getJoinPlanCacheDecoration =
    ServiceContext::declareDecoration<std::unique_ptr<JoinPlanCache>>();

ServiceContext::ConstructorActionRegisterer joinPlanCacheRegisterer{
    "JoinPlanCacheRegisterer", [](ServiceContext* serviceCtx) {
        getJoinPlanCacheDecoration(serviceCtx) = std::make_unique<JoinPlanCache>();
    }};

}  // namespace

std::shared_ptr<const JoinPlanCacheEntry> JoinPlanCache::lookup(const JoinPlanCacheKey& key) const {
    std::shared_lock lk(_mutex);
    auto it = _cache.find(key);
    return (it != _cache.end()) ? it->second : nullptr;
}

void JoinPlanCache::put(JoinPlanCacheKey key, std::unique_ptr<JoinPlanCacheEntry> entry) {
    tassert(12926501, "entry to join plan cache must not be null", entry);
    std::unique_lock lk(_mutex);
    _cache[std::move(key)] = std::move(entry);
}

void JoinPlanCache::remove(const JoinPlanCacheKey& key) {
    std::unique_lock lk(_mutex);
    _cache.erase(key);
}

JoinPlanCache& JoinPlanCache::get(ServiceContext* svc) {
    return *getJoinPlanCacheDecoration(svc);
}

std::vector<CollectionTag> makeCollectionTags(const MultipleCollectionAccessor& mca) {
    std::vector<CollectionTag> tags;
    mca.forEach([&](const CollectionPtr& collection) {
        tags.push_back(
            CollectionTag{collection->uuid(), JoinPlanCache::currentVersionTags(collection.get())});
    });
    return tags;
}

bool areCollectionTagsCurrent(const std::vector<CollectionTag>& tags,
                              const MultipleCollectionAccessor& mca) {
    // TODO (SERVER-130873): Simplify lookup once we have constant time access via UUID.
    for (const auto& tag : tags) {
        bool found = false;
        bool isCurrent = false;
        mca.forEach([&](const CollectionPtr& collection) {
            if (found || !collection || collection->uuid() != tag.uuid) {
                return;
            }
            found = true;
            isCurrent = (JoinPlanCache::currentVersionTags(collection.get()) == tag.versionTag);
        });
        if (!isCurrent) {
            LOGV2_DEBUG(12926600,
                        5,
                        "Detected stale join plan cache entry due to stale CollectionVersionTag",
                        "uuid"_attr = tag.uuid);
            return false;
        }
    }
    return true;
}

}  // namespace mongo
