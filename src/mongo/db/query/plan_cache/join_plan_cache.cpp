// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/plan_cache/join_plan_cache.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/container_size_helper.h"
#include "mongo/db/query/canonical_query_encoder.h"
#include "mongo/db/query/compiler/optimizer/join/join_method.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/db/query/util/memory_util.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/hex.h"
#include "mongo/util/overloaded_visitor.h"

#include <algorithm>
#include <mutex>
#include <variant>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace {

const ServiceContext::Decoration<std::unique_ptr<JoinPlanCache>> getJoinPlanCacheDecoration =
    ServiceContext::declareDecoration<std::unique_ptr<JoinPlanCache>>();

// Partition count, mirroring the SBE plan cache (sbe_plan_cache.cpp).
constexpr size_t kJoinPlanCacheNumPartitions = 32;

ServiceContext::ConstructorActionRegisterer joinPlanCacheRegisterer{
    "JoinPlanCacheRegisterer", [](ServiceContext* serviceCtx) {
        // Parse and cap the configured cache size, following the SBE plan cache pattern.
        auto status = memory_util::MemorySize::parse(internalQueryJoinPlanCacheSize.get());
        uassertStatusOK(status);
        auto requestedBytes = memory_util::getRequestedMemSizeInBytes(status.getValue());
        auto cappedBytes = memory_util::capMemorySize(
            requestedBytes, /*maximumSizeGB*/ 500, /*percentTotalSystemMemory*/ 25);
        getJoinPlanCacheDecoration(serviceCtx) =
            std::make_unique<JoinPlanCache>(cappedBytes, kJoinPlanCacheNumPartitions);
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

BSONObj CachedJoinPlan::toBSON() const {
    BSONObjBuilder bob;
    std::visit(OverloadedVisitor{
                   [&](const CachedAccessPath& node) {
                       bob.append("nodeId", static_cast<int>(node.nodeId));
                       if (node.solnCacheData) {
                           const auto& scd = *node.solnCacheData;
                           // SolutionCacheData::toString() dereferences 'tree' for index-based
                           // solutions, so only serialize the access path when it is well-formed.
                           const bool needsTree =
                               scd.solnType == SolutionCacheData::WHOLE_IXSCAN_SOLN ||
                               scd.solnType == SolutionCacheData::USE_INDEX_TAGS_SOLN;
                           if (!needsTree || scd.tree) {
                               bob.append("accessPath", scd.toString());
                           }
                       }
                   },
                   [&](const CachedInljNode& node) {
                       bob.append("nodeId", static_cast<int>(node.nodeId));
                       bob.append("inljForeignIndexName", node.inljForeignIndexName);
                   },
                   [&](const CachedJoinNode& node) {
                       bob.append("joinMethod", join_ordering::joinMethodToString(node.method));
                       BSONArrayBuilder predsBob(bob.subarrayStart("joinPredicates"));
                       for (const auto& pred : node.joinPredicates) {
                           predsBob.append(pred.toString());
                       }
                       predsBob.doneFast();
                       if (node.leftEmbeddingField) {
                           bob.append("leftEmbeddingField", node.leftEmbeddingField->fullPath());
                       }
                       if (node.rightEmbeddingField) {
                           bob.append("rightEmbeddingField", node.rightEmbeddingField->fullPath());
                       }
                       if (node.left) {
                           bob.append("left", node.left->toBSON());
                       }
                       if (node.right) {
                           bob.append("right", node.right->toBSON());
                       }
                   },
               },
               node);
    return bob.obj();
}

JoinPlanCacheEntry::JoinPlanCacheEntry(std::unique_ptr<CachedJoinPlan> joinTree,
                                       join_ordering::NodeId baseNode,
                                       std::vector<CollectionTag> collections,
                                       std::vector<NodeFingerprint> nodeFingerprints)
    : estimatedEntrySizeBytes(sizeof(JoinPlanCacheEntry) +
                              (joinTree ? joinTree->estimateObjectSizeInBytes() : 0) +
                              container_size_helper::estimateObjectSizeInBytes(collections) +
                              container_size_helper::estimateObjectSizeInBytes(nodeFingerprints)),
      joinTree(std::move(joinTree)),
      baseNode(baseNode),
      nodeFingerprints(std::move(nodeFingerprints)),
      _collections(std::move(collections)) {}

std::vector<CollectionTag> JoinPlanCacheEntry::getCollectionTags() const {
    std::lock_guard lk(_collectionsMutex);
    return _collections;
}

void JoinPlanCacheEntry::refreshCollectionTags(const std::vector<CollectionTag>& tags) {
    std::lock_guard lk(_collectionsMutex);

    tassert(13101000,
            "Refreshed join plan cache collection tags must cover the same number of collections",
            tags.size() == _collections.size());

    // TODO (SERVER-130873): Simplify lookup once we have constant time access via UUID.
    for (const auto& tag : tags) {
        auto cached = std::find_if(_collections.begin(),
                                   _collections.end(),
                                   [&](const CollectionTag& c) { return c.uuid == tag.uuid; });
        // Equal sizes plus a match for every incoming UUID means the two describe the same
        // collections, since a collection appears at most once in either.
        tassert(13101001,
                "Refreshed join plan cache collection tag matches no cached collection",
                cached != _collections.end());

        // Never move a collection version backwards. Two operations can revalidate this entry
        // concurrently, each against its own catalog snapshot, and a DDL running between them
        // leaves the older snapshot carrying a lower version.
        if (tag.versionTag.collectionVersion < cached->versionTag.collectionVersion) {
            continue;
        }
        cached->versionTag = tag.versionTag;
    }
}

std::shared_ptr<JoinPlanCacheEntry> JoinPlanCache::lookup(const JoinPlanCacheKey& key) const {
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
    return _cache.put(std::move(key), std::shared_ptr<JoinPlanCacheEntry>(std::move(entry)));
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

std::vector<BSONObj> JoinPlanCache::serializeEntries() const {
    // Gather entries into a vector to avoid holding the partition locks while serializing to BSON.
    std::vector<std::pair<JoinPlanCacheKey, std::shared_ptr<const JoinPlanCacheEntry>>> entries;
    _cache.forEach(
        [&](const JoinPlanCacheKey& key, const std::shared_ptr<const JoinPlanCacheEntry>& entry) {
            if (entry) {
                entries.push_back({key, entry});
            }
        });
    std::vector<BSONObj> results;
    results.reserve(entries.size());
    std::for_each(entries.begin(), entries.end(), [&results](auto& entry) {
        results.push_back(joinPlanCacheEntryToBSON(entry.first, *entry.second));
    });
    return results;
}

JoinPlanCache& JoinPlanCache::get(ServiceContext* svc) {
    return *getJoinPlanCacheDecoration(svc);
}

namespace join_ordering {
void bumpCollectionVersionForDDL(Collection* writableColl) {
    // Safe as a plain (non-atomic) increment: the caller holds the X lock and mutates the
    // copy-on-write Collection clone inside a WUOW, so the bumped value is only published on
    // commit and no reader can observe a torn value. The clone copy-constructs the decoration
    // from the currently-published Collection, so this increment yields 'previous + 1'.
    ++JoinPlanCache::currentVersionTags(writableColl).collectionVersion;
}
}  // namespace join_ordering

BSONObj joinPlanCacheEntryToBSON(const JoinPlanCacheKey& key, const JoinPlanCacheEntry& entry) {
    BSONObjBuilder bob;
    bob.append("planCacheKey", zeroPaddedHex(canonical_query_encoder::computeHash(key)));
    bob.append("baseNode", static_cast<int>(entry.baseNode));
    bob.append("estimatedSizeBytes", static_cast<long long>(entry.estimatedEntrySizeBytes));

    BSONArrayBuilder collectionsBob(bob.subarrayStart("collections"));
    for (const auto& tag : entry.getCollectionTags()) {
        BSONObjBuilder tagBob(collectionsBob.subobjStart());
        tag.uuid.appendToBuilder(&tagBob, "uuid");
        tagBob.append("collectionVersion",
                      static_cast<long long>(tag.versionTag.collectionVersion));
        tagBob.append("sampleVersion", static_cast<long long>(tag.versionTag.sampleVersion));
        tagBob.doneFast();
    }
    collectionsBob.doneFast();

    if (entry.joinTree) {
        bob.append("plan", entry.joinTree->toBSON());
    }
    return bob.obj();
}

std::vector<CollectionTag> makeCollectionTags(const MultipleCollectionAccessor& mca) {
    std::vector<CollectionTag> tags;
    mca.forEach([&](const CollectionPtr& collection) {
        tags.push_back(
            CollectionTag{collection->uuid(), JoinPlanCache::currentVersionTags(collection.get())});
    });
    return tags;
}

BSONObj collectionVersionsForLog(const std::vector<CollectionTag>& tags) {
    BSONObjBuilder builder;
    for (const auto& tag : tags) {
        builder.append(tag.uuid.toString(),
                       static_cast<long long>(tag.versionTag.collectionVersion));
    }
    return builder.obj();
}

CollectionTagStatus classifyCollectionTags(const std::vector<CollectionTag>& tags,
                                           const MultipleCollectionAccessor& mca) {
    auto status = CollectionTagStatus::kCurrent;

    // TODO (SERVER-130873): Simplify lookup once we have constant time access via UUID.
    for (const auto& tag : tags) {
        boost::optional<CollectionVersionTag> liveTag;
        mca.forEach([&](const CollectionPtr& collection) {
            if (liveTag || !collection || collection->uuid() != tag.uuid) {
                return;
            }
            liveTag = JoinPlanCache::currentVersionTags(collection.get());
        });

        if (!liveTag) {
            // The collection with the given UUID no longer exists, i.e. it was dropped.
            // This is the most restrictive status, so no other collection can change the outcome.
            LOGV2_DEBUG(12926600,
                        5,
                        "Join plan cache entry references a collection which no longer exists",
                        "uuid"_attr = tag.uuid);
            return CollectionTagStatus::kStale;
        }
        if (liveTag->collectionVersion != tag.versionTag.collectionVersion) {
            // A DDL ran, but it may have been on an index this plan could never use, which
            // the relevant-index fingerprints can settle.
            LOGV2_DEBUG(13036800,
                        5,
                        "Collection version bumped since the join plan was cached",
                        "uuid"_attr = tag.uuid,
                        "cachedVersion"_attr = tag.versionTag.collectionVersion,
                        "currentVersion"_attr = liveTag->collectionVersion);
            status = CollectionTagStatus::kNeedsIndexRevalidation;
        }
        // TODO (SERVER-129270): Return kStale when the persisted sample has been refreshed.
    }
    return status;
}

}  // namespace mongo
