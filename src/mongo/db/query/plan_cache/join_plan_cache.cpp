// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
