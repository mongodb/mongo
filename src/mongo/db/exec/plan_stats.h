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

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "mongo/db/exec/plan_stats_visitor.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/record_id_bound.h"
#include "mongo/db/query/stage_types.h"
#include "mongo/db/record_id.h"
#include "mongo/util/container_size_helper.h"
#include "mongo/util/time_support.h"

namespace mongo {

/**
 * The interface all specific-to-stage stats provide.
 */
struct SpecificStats {
    virtual ~SpecificStats() {}

    /**
     * Make a deep copy.
     */
    virtual std::unique_ptr<SpecificStats> clone() const = 0;

    virtual uint64_t estimateObjectSizeInBytes() const = 0;

    virtual void acceptVisitor(PlanStatsConstVisitor* visitor) const = 0;
    virtual void acceptVisitor(PlanStatsMutableVisitor* visitor) = 0;
};

// Every stage has CommonStats.
struct CommonStats {
    CommonStats() = delete;

    CommonStats(const char* type)
        : stageTypeStr(type),
          works(0),
          yields(0),
          unyields(0),
          advanced(0),
          needTime(0),
          needYield(0),
          failed(false),
          isEOF(false) {}

    uint64_t estimateObjectSizeInBytes() const {
        return filter.objsize() + sizeof(*this);
    }
    // String giving the type of the stage. Not owned.
    const char* stageTypeStr;

    // Count calls into the stage.
    size_t works;
    size_t yields;
    size_t unyields;

    // How many times was this state the return value of work(...)?
    size_t advanced;
    size_t needTime;
    size_t needYield;

    // BSON representation of a MatchExpression affixed to this node. If there
    // is no filter affixed, then 'filter' should be an empty BSONObj.
    BSONObj filter;

    // Time elapsed while working inside this stage. When this field is set to boost::none,
    // timing info will not be collected during query execution.
    //
    // The field must be populated when running explain or when running with the profiler on. It
    // must also be populated when multi planning, in order to gather stats stored in the plan
    // cache.
    boost::optional<Microseconds> executionTime;

    bool failed;
    bool isEOF;
};

// The universal container for a stage's stats.
template <typename C, typename T = void*>
struct BasePlanStageStats {
    BasePlanStageStats(const C& c, T t = {}) : stageType(t), common(c) {}

    /**
     * Make a deep copy.
     */
    BasePlanStageStats<C, T>* clone() const {
        auto stats = new BasePlanStageStats<C, T>(common, stageType);
        if (specific.get()) {
            stats->specific = specific->clone();
        }
        for (size_t i = 0; i < children.size(); ++i) {
            invariant(children[i]);
            stats->children.emplace_back(children[i]->clone());
        }
        return stats;
    }

    uint64_t estimateObjectSizeInBytes() const {
        return  // Add size of each element in 'children' vector.
            container_size_helper::estimateObjectSizeInBytes(
                children,
                [](const auto& child) { return child->estimateObjectSizeInBytes(); },
                true) +
            // Exclude the size of 'common' object since is being added later.
            (common.estimateObjectSizeInBytes() - sizeof(common)) +
            // Add 'specific' object size if exists.
            (specific ? specific->estimateObjectSizeInBytes() : 0) +
            // Add size of the object.
            sizeof(*this);
    }

    auto begin() {
        return children.begin();
    }

    auto begin() const {
        return children.begin();
    }

    auto end() {
        return children.end();
    }

    auto end() const {
        return children.end();
    }

    T stageType;

    // Stats exported by implementing the PlanStage interface.
    C common;

    // Per-stage place to stash additional information
    std::unique_ptr<SpecificStats> specific;

    // Per-stage additional debug info which is opaque to the caller. Callers should not attempt to
    // process/read this BSONObj other than for dumping the results to logs or back to the user.
    BSONObj debugInfo;

    // The stats of the node's children.
    std::vector<std::unique_ptr<BasePlanStageStats<C, T>>> children;

private:
    BasePlanStageStats(const BasePlanStageStats<C, T>&) = delete;
    BasePlanStageStats& operator=(const BasePlanStageStats<C, T>&) = delete;
};

using PlanStageStats = BasePlanStageStats<CommonStats, StageType>;

struct AndHashStats : public SpecificStats {
    AndHashStats() = default;

    std::unique_ptr<SpecificStats> clone() const final {
        return std::make_unique<AndHashStats>(*this);
    }

    uint64_t estimateObjectSizeInBytes() const {
        return container_size_helper::estimateObjectSizeInBytes(mapAfterChild) + sizeof(*this);
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    // How many entries are in the map after each child?
    // child 'i' produced children[i].common.advanced RecordIds, of which mapAfterChild[i] were
    // intersections.
    std::vector<size_t> mapAfterChild;

    // mapAfterChild[mapAfterChild.size() - 1] WSMswere match tested.
    // commonstats.advanced is how many passed.

    // What's our current memory usage?
    size_t memUsage = 0u;

    // What's our memory limit?
    size_t memLimit = 0u;
};

struct AndSortedStats : public SpecificStats {
    AndSortedStats() = default;

    std::unique_ptr<SpecificStats> clone() const final {
        return std::make_unique<AndSortedStats>(*this);
    }

    uint64_t estimateObjectSizeInBytes() const {
        return container_size_helper::estimateObjectSizeInBytes(failedAnd) + sizeof(*this);
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    // How many results from each child did not pass the AND?
    std::vector<size_t> failedAnd;
};

struct CachedPlanStats : public SpecificStats {
    CachedPlanStats() = default;

    std::unique_ptr<SpecificStats> clone() const final {
        return std::make_unique<CachedPlanStats>(*this);
    }

    uint64_t estimateObjectSizeInBytes() const {
        return sizeof(*this);
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    boost::optional<std::string> replanReason;
};

struct CollectionScanStats : public SpecificStats {
    CollectionScanStats() : docsTested(0), direction(1) {}

    std::unique_ptr<SpecificStats> clone() const final {
        return std::make_unique<CollectionScanStats>(*this);
    }

    uint64_t estimateObjectSizeInBytes() const {
        return sizeof(*this);
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    // How many documents did we check against our filter?
    size_t docsTested;

    // >0 if we're traversing the collection forwards. <0 if we're traversing it
    // backwards.
    int direction;

    bool tailable{false};

    // The start location of a forward scan and end location for a reverse scan.
    boost::optional<RecordIdBound> minRecord;

    // The end location of a reverse scan and start location for a forward scan.
    boost::optional<RecordIdBound> maxRecord;
};

struct CountStats : public SpecificStats {
    CountStats() : nCounted(0), nSkipped(0) {}

    std::unique_ptr<SpecificStats> clone() const final {
        return std::make_unique<CountStats>(*this);
    }

    uint64_t estimateObjectSizeInBytes() const {
        return sizeof(*this);
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    // The result of the count.
    long long nCounted;

    // The number of results we skipped over.
    long long nSkipped;
};

struct CountScanStats : public SpecificStats {
    CountScanStats()
        : indexVersion(0),
          isMultiKey(false),
          isPartial(false),
          isSparse(false),
          isUnique(false),
          keysExamined(0) {}

    std::unique_ptr<SpecificStats> clone() const final {
        auto specific = std::make_unique<CountScanStats>(*this);
        // BSON objects have to be explicitly copied.
        specific->keyPattern = keyPattern.getOwned();
        specific->collation = collation.getOwned();
        specific->startKey = startKey.getOwned();
        specific->endKey = endKey.getOwned();
        return specific;
    }

    uint64_t estimateObjectSizeInBytes() const {
        return container_size_helper::estimateObjectSizeInBytes(
                   multiKeyPaths,
                   [](const auto& keyPath) {
                       // Calculate the size of each std::set in 'multiKeyPaths'.
                       return container_size_helper::estimateObjectSizeInBytes(keyPath);
                   },
                   true) +
            keyPattern.objsize() + collation.objsize() + startKey.objsize() + endKey.objsize() +
            indexName.capacity() + sizeof(*this);
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    std::string indexName;

    BSONObj keyPattern;

    BSONObj collation;

    // The starting/ending key(s) of the index scan.
    // startKey and endKey contain the fields of keyPattern, with values
    // that match the corresponding index bounds.
    BSONObj startKey;
    BSONObj endKey;
    // Whether or not those keys are inclusive or exclusive bounds.
    bool startKeyInclusive;
    bool endKeyInclusive;

    int indexVersion;

    // Set to true if the index used for the count scan is multikey.
    bool isMultiKey;

    // Represents which prefixes of the indexed field(s) cause the index to be multikey.
    MultikeyPaths multiKeyPaths;

    bool isPartial;
    bool isSparse;
    bool isUnique;

    size_t keysExamined;
};

struct DeleteStats : public SpecificStats {
    DeleteStats() = default;

    std::unique_ptr<SpecificStats> clone() const final {
        return std::make_unique<DeleteStats>(*this);
    }

    uint64_t estimateObjectSizeInBytes() const {
        return sizeof(*this);
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    size_t docsDeleted = 0u;
    size_t bytesDeleted = 0u;
};

struct BatchedDeleteStats : public DeleteStats {
    BatchedDeleteStats() = default;

    // Unlike a standard multi:true delete, BatchedDeleteStage can complete with PlanStage::IS_EOF
    // before deleting all the documents that match the query, if a 'BatchedDeleteStageParams'
    // 'pass' target is met.
    //
    // True indicates the operation reaches completion because a 'pass' target is met.
    //
    // False indicates either (1) the operation hasn't reached completion or (2) the operation
    // removed all the documents that matched the criteria to reach completion - this is always the
    // case with the default 'BatchedDeleteStageParams'.
    bool passTargetMet = false;
};

struct DistinctScanStats : public SpecificStats {
    std::unique_ptr<SpecificStats> clone() const final {
        auto specific = std::make_unique<DistinctScanStats>(*this);
        // BSON objects have to be explicitly copied.
        specific->keyPattern = keyPattern.getOwned();
        specific->collation = collation.getOwned();
        specific->indexBounds = indexBounds.getOwned();
        return specific;
    }

    uint64_t estimateObjectSizeInBytes() const {
        return container_size_helper::estimateObjectSizeInBytes(
                   multiKeyPaths,
                   [](const auto& keyPath) {
                       // Calculate the size of each std::set in 'multiKeyPaths'.
                       return container_size_helper::estimateObjectSizeInBytes(keyPath);
                   },
                   true) +
            keyPattern.objsize() + collation.objsize() + indexBounds.objsize() +
            indexName.capacity() + sizeof(*this);
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    // How many keys did we look at while distinct-ing?
    size_t keysExamined = 0;

    BSONObj keyPattern;

    BSONObj collation;

    // Properties of the index used for the distinct scan.
    std::string indexName;
    int indexVersion = 0;

    // Set to true if the index used for the distinct scan is multikey.
    bool isMultiKey = false;

    // Represents which prefixes of the indexed field(s) cause the index to be multikey.
    MultikeyPaths multiKeyPaths;

    bool isPartial = false;
    bool isSparse = false;
    bool isUnique = false;

    // >1 if we're traversing the index forwards and <1 if we're traversing it backwards.
    int direction = 1;

    // A BSON representation of the distinct scan's index bounds.
    BSONObj indexBounds;
};

struct FetchStats : public SpecificStats {
    FetchStats() = default;

    std::unique_ptr<SpecificStats> clone() const final {
        return std::make_unique<FetchStats>(*this);
    }

    uint64_t estimateObjectSizeInBytes() const {
        return sizeof(*this);
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    // Have we seen anything that already had an object?
    size_t alreadyHasObj = 0u;

    // The total number of full documents touched by the fetch stage.
    size_t docsExamined = 0u;
};

struct IDHackStats : public SpecificStats {
    IDHackStats() : keysExamined(0), docsExamined(0) {}

    std::unique_ptr<SpecificStats> clone() const final {
        return std::make_unique<IDHackStats>(*this);
    }

    uint64_t estimateObjectSizeInBytes() const {
        return indexName.capacity() + sizeof(*this);
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    std::string indexName;

    // Number of entries retrieved from the index while executing the idhack.
    size_t keysExamined;

    // Number of documents retrieved from the collection while executing the idhack.
    size_t docsExamined;
};

struct ReturnKeyStats : public SpecificStats {
    std::unique_ptr<SpecificStats> clone() const final {
        return std::make_unique<ReturnKeyStats>(*this);
    }

    uint64_t estimateObjectSizeInBytes() const {
        return sizeof(*this);
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) final {
        visitor->visit(this);
    }
};

struct IndexScanStats : public SpecificStats {
    IndexScanStats()
        : indexVersion(0),
          direction(1),
          isMultiKey(false),
          isPartial(false),
          isSparse(false),
          isUnique(false),
          dupsTested(0),
          dupsDropped(0),
          keysExamined(0),
          seeks(0) {}

    std::unique_ptr<SpecificStats> clone() const final {
        auto specific = std::make_unique<IndexScanStats>(*this);
        // BSON objects have to be explicitly copied.
        specific->keyPattern = keyPattern.getOwned();
        specific->collation = collation.getOwned();
        specific->indexBounds = indexBounds.getOwned();
        return specific;
    }

    uint64_t estimateObjectSizeInBytes() const {
        return container_size_helper::estimateObjectSizeInBytes(
                   multiKeyPaths,
                   [](const auto& keyPath) {
                       // Calculate the size of each std::set in 'multiKeyPaths'.
                       return container_size_helper::estimateObjectSizeInBytes(keyPath);
                   },
                   true) +
            keyPattern.objsize() + collation.objsize() + indexBounds.objsize() +
            indexName.capacity() + indexType.capacity() + sizeof(*this);
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    // Index type being used.
    std::string indexType;

    // name of the index being used
    std::string indexName;

    BSONObj keyPattern;

    BSONObj collation;

    int indexVersion;

    // A BSON (opaque, ie. hands off other than toString() it) representation of the bounds
    // used.
    BSONObj indexBounds;

    // >1 if we're traversing the index along with its order. <1 if we're traversing it
    // against the order.
    int direction;

    // index properties
    // Whether this index is over a field that contain array values.
    bool isMultiKey;

    // Represents which prefixes of the indexed field(s) cause the index to be multikey.
    MultikeyPaths multiKeyPaths;

    bool isPartial;
    bool isSparse;
    bool isUnique;

    size_t dupsTested;
    size_t dupsDropped;

    // Number of entries retrieved from the index during the scan.
    size_t keysExamined;

    // Number of times the index cursor is re-positioned during the execution of the scan.
    size_t seeks;
};

struct LimitStats : public SpecificStats {
    LimitStats() : limit(0) {}

    std::unique_ptr<SpecificStats> clone() const final {
        return std::make_unique<LimitStats>(*this);
    }

    uint64_t estimateObjectSizeInBytes() const {
        return sizeof(*this);
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    size_t limit;
};

struct MockStats : public SpecificStats {
    MockStats() {}

    std::unique_ptr<SpecificStats> clone() const final {
        return std::make_unique<MockStats>(*this);
    }

    uint64_t estimateObjectSizeInBytes() const {
        return sizeof(*this);
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) final {
        visitor->visit(this);
    }
};

struct MultiPlanStats : public SpecificStats {
    MultiPlanStats() {}

    std::unique_ptr<SpecificStats> clone() const final {
        return std::make_unique<MultiPlanStats>(*this);
    }

    uint64_t estimateObjectSizeInBytes() const {
        return sizeof(*this);
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) final {
        visitor->visit(this);
    }
};

struct OrStats : public SpecificStats {
    OrStats() = default;

    std::unique_ptr<SpecificStats> clone() const final {
        return std::make_unique<OrStats>(*this);
    }

    uint64_t estimateObjectSizeInBytes() const {
        return sizeof(*this);
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    size_t dupsTested = 0u;
    size_t dupsDropped = 0u;
};

struct ProjectionStats : public SpecificStats {
    ProjectionStats() {}

    std::unique_ptr<SpecificStats> clone() const final {
        return std::make_unique<ProjectionStats>(*this);
    }

    uint64_t estimateObjectSizeInBytes() const {
        return projObj.objsize() + sizeof(*this);
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    // Object specifying the projection transformation to apply.
    BSONObj projObj;
};

struct SortStats : public SpecificStats {
    SortStats() = default;
    SortStats(uint64_t limit, uint64_t maxMemoryUsageBytes)
        : limit(limit), maxMemoryUsageBytes(maxMemoryUsageBytes) {}

    std::unique_ptr<SpecificStats> clone() const {
        return std::make_unique<SortStats>(*this);
    }

    uint64_t estimateObjectSizeInBytes() const {
        return sortPattern.objsize() + sizeof(*this);
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    // The pattern according to which we are sorting.
    BSONObj sortPattern;

    // The number of results to return from the sort.
    uint64_t limit = 0u;

    // The maximum number of bytes of memory we're willing to use during execution of the sort. If
    // this limit is exceeded and 'allowDiskUse' is false, the query will fail at execution time. If
    // 'allowDiskUse' is true, the data will be spilled to disk.
    uint64_t maxMemoryUsageBytes = 0u;

    // The amount of data we've sorted in bytes. At various times this data may be buffered in
    // memory or disk-resident, depending on the configuration of 'maxMemoryUsageBytes' and whether
    // disk use is allowed.
    uint64_t totalDataSizeBytes = 0u;

    // The number of keys that we've sorted.
    uint64_t keysSorted = 0u;

    // The number of times that we spilled data to disk during the execution of this query.
    uint64_t spills = 0u;
};

struct MergeSortStats : public SpecificStats {
    MergeSortStats() = default;

    std::unique_ptr<SpecificStats> clone() const final {
        return std::make_unique<MergeSortStats>(*this);
    }

    uint64_t estimateObjectSizeInBytes() const {
        return sortPattern.objsize() + sizeof(*this);
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    size_t dupsTested = 0u;
    size_t dupsDropped = 0u;

    // The pattern according to which we are sorting.
    BSONObj sortPattern;
};

struct ShardingFilterStats : public SpecificStats {
    ShardingFilterStats() : chunkSkips(0) {}

    std::unique_ptr<SpecificStats> clone() const final {
        return std::make_unique<ShardingFilterStats>(*this);
    }

    uint64_t estimateObjectSizeInBytes() const {
        return sizeof(*this);
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    size_t chunkSkips;
};

struct SkipStats : public SpecificStats {
    SkipStats() : skip(0) {}

    std::unique_ptr<SpecificStats> clone() const final {
        return std::make_unique<SkipStats>(*this);
    }

    uint64_t estimateObjectSizeInBytes() const {
        return sizeof(*this);
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    size_t skip;
};

struct IntervalStats {
    // Number of results found in the covering of this interval.
    long long numResultsBuffered = 0;
    // Number of documents in this interval returned to the parent stage.
    long long numResultsReturned = 0;

    // Min distance of this interval - always inclusive.
    double minDistanceAllowed = -1;
    // Max distance of this interval - inclusive iff inclusiveMaxDistanceAllowed.
    double maxDistanceAllowed = -1;
    // True only in the last interval.
    bool inclusiveMaxDistanceAllowed = false;
};

struct NearStats : public SpecificStats {
    NearStats() : indexVersion(0) {}

    std::unique_ptr<SpecificStats> clone() const final {
        return std::make_unique<NearStats>(*this);
    }

    uint64_t estimateObjectSizeInBytes() const {
        return container_size_helper::estimateObjectSizeInBytes(intervalStats) +
            keyPattern.objsize() + indexName.capacity() + sizeof(*this);
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    std::vector<IntervalStats> intervalStats;
    std::string indexName;
    // btree index version, not geo index version
    int indexVersion;
    BSONObj keyPattern;
};

struct UpdateStats : public SpecificStats {
    UpdateStats() : nMatched(0), nModified(0), isModUpdate(false), nUpserted(0) {}

    std::unique_ptr<SpecificStats> clone() const final {
        return std::make_unique<UpdateStats>(*this);
    }

    uint64_t estimateObjectSizeInBytes() const {
        return objInserted.objsize() + sizeof(*this);
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    // The number of documents which match the query part of the update.
    size_t nMatched;

    // The number of documents modified by this update.
    size_t nModified;

    // True iff this is a $mod update.
    bool isModUpdate;

    // Will be 1 if this is an {upsert: true} update that did an insert, 0 otherwise.
    size_t nUpserted;

    // The object that was inserted. This is an empty document if no insert was performed.
    BSONObj objInserted;
};

struct TextMatchStats : public SpecificStats {
    std::unique_ptr<SpecificStats> clone() const final {
        return std::make_unique<TextMatchStats>(*this);
    }

    uint64_t estimateObjectSizeInBytes() const {
        return parsedTextQuery.objsize() + indexPrefix.objsize() + indexName.capacity() +
            sizeof(*this);
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    std::string indexName;

    // Human-readable form of the FTSQuery associated with the text stage.
    BSONObj parsedTextQuery;

    int textIndexVersion{0};

    // Index keys that precede the "text" index key.
    BSONObj indexPrefix;

    size_t docsRejected{0};
};

struct TextOrStats : public SpecificStats {
    TextOrStats() : fetches(0) {}

    std::unique_ptr<SpecificStats> clone() const final {
        return std::make_unique<TextOrStats>(*this);
    }

    uint64_t estimateObjectSizeInBytes() const {
        return sizeof(*this);
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    size_t fetches;
};

struct TrialStats : public SpecificStats {
    std::unique_ptr<SpecificStats> clone() const final {
        return std::make_unique<TrialStats>(*this);
    }

    uint64_t estimateObjectSizeInBytes() const {
        return sizeof(*this);
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    size_t trialPeriodMaxWorks = 0;
    double successThreshold = 0;

    size_t trialWorks = 0;
    size_t trialAdvanced = 0;

    bool trialCompleted = false;
    bool trialSucceeded = false;
};

struct GroupStats : public SpecificStats {
    std::unique_ptr<SpecificStats> clone() const final {
        return std::make_unique<GroupStats>(*this);
    }

    uint64_t estimateObjectSizeInBytes() const {
        return sizeof(*this);
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    // Tracks an estimate of the total size of all documents output by the group stage in bytes.
    size_t totalOutputDataSizeBytes = 0;

    // The number of times that we spilled data to disk while grouping the data.
    uint64_t spills = 0u;
};

struct DocumentSourceCursorStats : public SpecificStats {
    std::unique_ptr<SpecificStats> clone() const final {
        return std::make_unique<DocumentSourceCursorStats>(*this);
    }

    uint64_t estimateObjectSizeInBytes() const {
        return sizeof(*this) +
            (planSummaryStats.estimateObjectSizeInBytes() - sizeof(planSummaryStats));
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    PlanSummaryStats planSummaryStats;
};

struct DocumentSourceLookupStats : public SpecificStats {
    std::unique_ptr<SpecificStats> clone() const final {
        return std::make_unique<DocumentSourceLookupStats>(*this);
    }

    uint64_t estimateObjectSizeInBytes() const {
        return sizeof(*this) +
            (planSummaryStats.estimateObjectSizeInBytes() - sizeof(planSummaryStats));
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    // Tracks the summary stats in aggregate across all executions of the subpipeline.
    PlanSummaryStats planSummaryStats;
};

struct UnionWithStats final : public SpecificStats {
    std::unique_ptr<SpecificStats> clone() const final {
        return std::make_unique<UnionWithStats>(*this);
    }

    uint64_t estimateObjectSizeInBytes() const {
        return sizeof(*this) +
            (planSummaryStats.estimateObjectSizeInBytes() - sizeof(planSummaryStats));
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    // Tracks the summary stats of the subpipeline.
    PlanSummaryStats planSummaryStats;
};

struct DocumentSourceFacetStats : public SpecificStats {
    std::unique_ptr<SpecificStats> clone() const final {
        return std::make_unique<DocumentSourceFacetStats>(*this);
    }

    uint64_t estimateObjectSizeInBytes() const {
        return sizeof(*this) +
            (planSummaryStats.estimateObjectSizeInBytes() - sizeof(planSummaryStats));
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    // Tracks the cumulative summary stats across all facets.
    PlanSummaryStats planSummaryStats;
};

struct UnpackTimeseriesBucketStats final : public SpecificStats {
    std::unique_ptr<SpecificStats> clone() const final {
        return std::make_unique<UnpackTimeseriesBucketStats>(*this);
    }

    uint64_t estimateObjectSizeInBytes() const {
        return sizeof(*this);
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    size_t nBucketsUnpacked = 0u;
};

struct SampleFromTimeseriesBucketStats final : public SpecificStats {
    std::unique_ptr<SpecificStats> clone() const final {
        return std::make_unique<SampleFromTimeseriesBucketStats>(*this);
    }

    uint64_t estimateObjectSizeInBytes() const {
        return sizeof(*this);
    }

    void acceptVisitor(PlanStatsConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    void acceptVisitor(PlanStatsMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    size_t nBucketsDiscarded = 0u;
    size_t dupsTested = 0u;
    size_t dupsDropped = 0u;
};
}  // namespace mongo
