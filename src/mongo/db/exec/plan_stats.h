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

#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/query/stage_types.h"
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
    virtual SpecificStats* clone() const = 0;

    virtual uint64_t estimateObjectSizeInBytes() const = 0;
};

// Every stage has CommonStats.
struct CommonStats {
    CommonStats(const char* type)
        : stageTypeStr(type),
          works(0),
          yields(0),
          unyields(0),
          advanced(0),
          needTime(0),
          needYield(0),
          executionTimeMillis(0),
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

    // Time elapsed while working inside this stage.
    long long executionTimeMillis;

    // TODO: have some way of tracking WSM sizes (or really any series of #s).  We can measure
    // the size of our inputs and the size of our outputs.  We can do a lot with the WS here.

    // TODO: once we've picked a plan, collect different (or additional) stats for display to
    // the user, eg. time_t totalTimeSpent;

    // TODO: keep track of the total yield time / fetch time done for a plan.

    bool isEOF;

private:
    // Default constructor is illegal.
    CommonStats();
};

// The universal container for a stage's stats.
struct PlanStageStats {
    PlanStageStats(const CommonStats& c, StageType t) : stageType(t), common(c) {}

    /**
     * Make a deep copy.
     */
    PlanStageStats* clone() const {
        PlanStageStats* stats = new PlanStageStats(common, stageType);
        if (specific.get()) {
            stats->specific.reset(specific->clone());
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

    // See query/stage_type.h
    StageType stageType;

    // Stats exported by implementing the PlanStage interface.
    CommonStats common;

    // Per-stage place to stash additional information
    std::unique_ptr<SpecificStats> specific;

    // The stats of the node's children.
    std::vector<std::unique_ptr<PlanStageStats>> children;

private:
    PlanStageStats(const PlanStageStats&) = delete;
    PlanStageStats& operator=(const PlanStageStats&) = delete;
};

struct AndHashStats : public SpecificStats {
    AndHashStats() = default;

    SpecificStats* clone() const final {
        AndHashStats* specific = new AndHashStats(*this);
        return specific;
    }

    uint64_t estimateObjectSizeInBytes() const {
        return container_size_helper::estimateObjectSizeInBytes(mapAfterChild) + sizeof(*this);
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

    SpecificStats* clone() const final {
        AndSortedStats* specific = new AndSortedStats(*this);
        return specific;
    }

    uint64_t estimateObjectSizeInBytes() const {
        return container_size_helper::estimateObjectSizeInBytes(failedAnd) + sizeof(*this);
    }

    // How many results from each child did not pass the AND?
    std::vector<size_t> failedAnd;
};

struct CachedPlanStats : public SpecificStats {
    CachedPlanStats() : replanned(false) {}

    SpecificStats* clone() const final {
        return new CachedPlanStats(*this);
    }

    uint64_t estimateObjectSizeInBytes() const {
        return sizeof(*this);
    }

    bool replanned;
};

struct CollectionScanStats : public SpecificStats {
    CollectionScanStats() : docsTested(0), direction(1) {}

    SpecificStats* clone() const final {
        CollectionScanStats* specific = new CollectionScanStats(*this);
        return specific;
    }

    uint64_t estimateObjectSizeInBytes() const {
        return sizeof(*this);
    }


    // How many documents did we check against our filter?
    size_t docsTested;

    // >0 if we're traversing the collection forwards. <0 if we're traversing it
    // backwards.
    int direction;

    // If present, indicates that the collection scan will stop and return EOF the first time it
    // sees a document that does not pass the filter and has a "ts" Timestamp field greater than
    // 'maxTs'.
    boost::optional<Timestamp> maxTs;
};

struct CountStats : public SpecificStats {
    CountStats() : nCounted(0), nSkipped(0) {}

    SpecificStats* clone() const final {
        CountStats* specific = new CountStats(*this);
        return specific;
    }

    uint64_t estimateObjectSizeInBytes() const {
        return sizeof(*this);
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

    SpecificStats* clone() const final {
        CountScanStats* specific = new CountScanStats(*this);
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

    SpecificStats* clone() const final {
        return new DeleteStats(*this);
    }

    uint64_t estimateObjectSizeInBytes() const {
        return sizeof(*this);
    }

    size_t docsDeleted = 0u;
};

struct DistinctScanStats : public SpecificStats {
    SpecificStats* clone() const final {
        DistinctScanStats* specific = new DistinctScanStats(*this);
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

struct EnsureSortedStats : public SpecificStats {
    EnsureSortedStats() : nDropped(0) {}

    SpecificStats* clone() const final {
        EnsureSortedStats* specific = new EnsureSortedStats(*this);
        return specific;
    }

    uint64_t estimateObjectSizeInBytes() const {
        return sizeof(*this);
    }

    // The number of out-of-order results that were dropped.
    long long nDropped;
};

struct FetchStats : public SpecificStats {
    FetchStats() = default;

    SpecificStats* clone() const final {
        FetchStats* specific = new FetchStats(*this);
        return specific;
    }

    uint64_t estimateObjectSizeInBytes() const {
        return sizeof(*this);
    }

    // Have we seen anything that already had an object?
    size_t alreadyHasObj = 0u;

    // The total number of full documents touched by the fetch stage.
    size_t docsExamined = 0u;
};

struct IDHackStats : public SpecificStats {
    IDHackStats() : keysExamined(0), docsExamined(0) {}

    SpecificStats* clone() const final {
        IDHackStats* specific = new IDHackStats(*this);
        return specific;
    }

    uint64_t estimateObjectSizeInBytes() const {
        return indexName.capacity() + sizeof(*this);
    }

    std::string indexName;

    // Number of entries retrieved from the index while executing the idhack.
    size_t keysExamined;

    // Number of documents retrieved from the collection while executing the idhack.
    size_t docsExamined;
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

    SpecificStats* clone() const final {
        IndexScanStats* specific = new IndexScanStats(*this);
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

    SpecificStats* clone() const final {
        LimitStats* specific = new LimitStats(*this);
        return specific;
    }

    uint64_t estimateObjectSizeInBytes() const {
        return sizeof(*this);
    }

    size_t limit;
};

struct MockStats : public SpecificStats {
    MockStats() {}

    SpecificStats* clone() const final {
        return new MockStats(*this);
    }

    uint64_t estimateObjectSizeInBytes() const {
        return sizeof(*this);
    }
};

struct MultiPlanStats : public SpecificStats {
    MultiPlanStats() {}

    SpecificStats* clone() const final {
        return new MultiPlanStats(*this);
    }

    uint64_t estimateObjectSizeInBytes() const {
        return sizeof(*this);
    }
};

struct OrStats : public SpecificStats {
    OrStats() = default;

    SpecificStats* clone() const final {
        OrStats* specific = new OrStats(*this);
        return specific;
    }

    uint64_t estimateObjectSizeInBytes() const {
        return sizeof(*this);
    }

    size_t dupsTested = 0u;
    size_t dupsDropped = 0u;
};

struct ProjectionStats : public SpecificStats {
    ProjectionStats() {}

    SpecificStats* clone() const final {
        ProjectionStats* specific = new ProjectionStats(*this);
        return specific;
    }

    uint64_t estimateObjectSizeInBytes() const {
        return projObj.objsize() + sizeof(*this);
    }

    // Object specifying the projection transformation to apply.
    BSONObj projObj;
};

struct SortStats : public SpecificStats {
    SortStats() = default;

    SpecificStats* clone() const final {
        SortStats* specific = new SortStats(*this);
        return specific;
    }

    uint64_t estimateObjectSizeInBytes() const {
        return sortPattern.objsize() + sizeof(*this);
    }

    // What's our current memory usage?
    size_t memUsage = 0u;

    // What's our memory limit?
    size_t memLimit = 0u;

    // The number of results to return from the sort.
    size_t limit = 0u;

    // The pattern according to which we are sorting.
    BSONObj sortPattern;
};

struct MergeSortStats : public SpecificStats {
    MergeSortStats() = default;

    SpecificStats* clone() const final {
        MergeSortStats* specific = new MergeSortStats(*this);
        return specific;
    }

    uint64_t estimateObjectSizeInBytes() const {
        return sortPattern.objsize() + sizeof(*this);
    }

    size_t dupsTested = 0u;
    size_t dupsDropped = 0u;

    // The pattern according to which we are sorting.
    BSONObj sortPattern;
};

struct ShardingFilterStats : public SpecificStats {
    ShardingFilterStats() : chunkSkips(0) {}

    SpecificStats* clone() const final {
        ShardingFilterStats* specific = new ShardingFilterStats(*this);
        return specific;
    }

    uint64_t estimateObjectSizeInBytes() const {
        return sizeof(*this);
    }

    size_t chunkSkips;
};

struct SkipStats : public SpecificStats {
    SkipStats() : skip(0) {}

    SpecificStats* clone() const final {
        SkipStats* specific = new SkipStats(*this);
        return specific;
    }

    uint64_t estimateObjectSizeInBytes() const {
        return sizeof(*this);
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

    SpecificStats* clone() const final {
        return new NearStats(*this);
    }

    uint64_t estimateObjectSizeInBytes() const {
        return container_size_helper::estimateObjectSizeInBytes(intervalStats) +
            keyPattern.objsize() + indexName.capacity() + sizeof(*this);
    }

    std::vector<IntervalStats> intervalStats;
    std::string indexName;
    // btree index version, not geo index version
    int indexVersion;
    BSONObj keyPattern;
};

struct UpdateStats : public SpecificStats {
    UpdateStats() : nMatched(0), nModified(0), isModUpdate(false), inserted(false) {}

    SpecificStats* clone() const final {
        return new UpdateStats(*this);
    }

    uint64_t estimateObjectSizeInBytes() const {
        return objInserted.objsize() + sizeof(*this);
    }

    // The number of documents which match the query part of the update.
    size_t nMatched;

    // The number of documents modified by this update.
    size_t nModified;

    // True iff this is a $mod update.
    bool isModUpdate;

    // Is this an {upsert: true} update that did an insert?
    bool inserted;

    // The object that was inserted. This is an empty document if no insert was performed.
    BSONObj objInserted;
};

struct TextStats : public SpecificStats {
    TextStats() : parsedTextQuery(), textIndexVersion(0) {}

    SpecificStats* clone() const final {
        TextStats* specific = new TextStats(*this);
        return specific;
    }

    uint64_t estimateObjectSizeInBytes() const {
        return parsedTextQuery.objsize() + indexPrefix.objsize() + indexName.capacity() +
            sizeof(*this);
    }

    std::string indexName;

    // Human-readable form of the FTSQuery associated with the text stage.
    BSONObj parsedTextQuery;

    int textIndexVersion;

    // Index keys that precede the "text" index key.
    BSONObj indexPrefix;
};

struct TextMatchStats : public SpecificStats {
    TextMatchStats() : docsRejected(0) {}

    SpecificStats* clone() const final {
        TextMatchStats* specific = new TextMatchStats(*this);
        return specific;
    }

    uint64_t estimateObjectSizeInBytes() const {
        return sizeof(*this);
    }

    size_t docsRejected;
};

struct TextOrStats : public SpecificStats {
    TextOrStats() : fetches(0) {}

    SpecificStats* clone() const final {
        TextOrStats* specific = new TextOrStats(*this);
        return specific;
    }

    uint64_t estimateObjectSizeInBytes() const {
        return sizeof(*this);
    }

    size_t fetches;
};

struct TrialStats : public SpecificStats {
    SpecificStats* clone() const final {
        TrialStats* specific = new TrialStats(*this);
        return specific;
    }

    uint64_t estimateObjectSizeInBytes() const {
        return sizeof(*this);
    }

    size_t trialPeriodMaxWorks = 0;
    double successThreshold = 0;

    size_t trialWorks = 0;
    size_t trialAdvanced = 0;

    bool trialCompleted = false;
    bool trialSucceeded = false;
};

}  // namespace mongo
