// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/plan_cache/join_plan_cache.h"

#include "mongo/db/exec/container_size_helper.h"
#include "mongo/logv2/log.h"

#include <variant>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace {

const ServiceContext::Decoration<std::unique_ptr<JoinPlanCache>> getJoinPlanCacheDecoration =
    ServiceContext::declareDecoration<std::unique_ptr<JoinPlanCache>>();

// Default to 5MiB
constexpr size_t kDefaultJoinPlanCacheSizeBytes = 5 * 1024 * 1024;
constexpr size_t kDefaultJoinPlanCacheNumPartitions = 32;

ServiceContext::ConstructorActionRegisterer joinPlanCacheRegisterer{
    "JoinPlanCacheRegisterer", [](ServiceContext* serviceCtx) {
        getJoinPlanCacheDecoration(serviceCtx) = std::make_unique<JoinPlanCache>(
            kDefaultJoinPlanCacheSizeBytes, kDefaultJoinPlanCacheNumPartitions);
    }};

// Heap bytes backing a FieldPath.
size_t estimateFieldPathHeapBytes(const FieldPath& fieldPath) {
    // Capture FieldPath::_fieldPath string bytes
    size_t fieldPathHeapBytes = fieldPath.fullPath().capacity() * sizeof(char);
    // Capture FieldPath::_fieldPathDotPosition vector bytes
    size_t fieldPathDotVectorHeapBytes = fieldPath.getPathLength() * sizeof(size_t);
    return fieldPathHeapBytes + fieldPathDotVectorHeapBytes;
}

}  // namespace

size_t CachedAccessPath::estimateHeapBytes() const {
    return solnCacheData ? solnCacheData->estimateObjectSizeInBytes() : 0;
}

size_t CachedInljNode::estimateHeapBytes() const {
    return inljForeignIndexName.capacity();
}

size_t CachedJoinNode::estimateHeapBytes() const {
    size_t size = container_size_helper::estimateObjectSizeInBytes(
        joinPredicates,
        [](const QSNJoinPredicate& pred) {
            return estimateFieldPathHeapBytes(pred.leftField) +
                estimateFieldPathHeapBytes(pred.rightField);
        },
        /*includeShallowSize*/ true);
    if (leftEmbeddingField) {
        size += estimateFieldPathHeapBytes(*leftEmbeddingField);
    }
    if (rightEmbeddingField) {
        size += estimateFieldPathHeapBytes(*rightEmbeddingField);
    }
    size += left ? left->estimateObjectSizeInBytes() : 0;
    size += right ? right->estimateObjectSizeInBytes() : 0;
    return size;
}

size_t CachedJoinPlan::estimateObjectSizeInBytes() const {
    return sizeof(CachedJoinPlan) +
        std::visit([](const auto& n) { return n.estimateHeapBytes(); }, node);
}

JoinPlanCacheEntry::JoinPlanCacheEntry(std::unique_ptr<CachedJoinPlan> joinTree,
                                       join_ordering::NodeId baseNode,
                                       std::vector<CollectionTag> collections)
    : joinTree(std::move(joinTree)),
      baseNode(baseNode),
      collections(std::move(collections)),
      estimatedEntrySizeBytes(sizeof(JoinPlanCacheEntry) +
                              (this->joinTree ? this->joinTree->estimateObjectSizeInBytes() : 0)) {}

std::shared_ptr<const JoinPlanCacheEntry> JoinPlanCache::lookup(const JoinPlanCacheKey& key) const {
    // Hold the partition lock while copying out the shared_ptr because PartitionedCache::lookup()
    // releases its lock before returning.
    auto [swEntry, partitionLock] = _cache.getWithPartitionLock(key);
    if (!swEntry.isOK()) {
        return nullptr;
    }
    return *swEntry.getValue();
}

size_t JoinPlanCache::put(JoinPlanCacheKey key, std::unique_ptr<JoinPlanCacheEntry> entry) {
    tassert(12926501, "entry to join plan cache must not be null", entry);
    return _cache.put(std::move(key), std::shared_ptr<const JoinPlanCacheEntry>(std::move(entry)));
}

void JoinPlanCache::remove(const JoinPlanCacheKey& key) {
    _cache.remove(key);
}

size_t JoinPlanCache::reset(size_t cacheSizeBytes) {
    return _cache.reset(cacheSizeBytes);
}

size_t JoinPlanCache::size() const {
    return _cache.size();
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
