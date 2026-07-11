// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/recordid_deduplicator.h"
#include "mongo/db/exec/classic/requires_index_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/compiler/physical_model/index_bounds/index_bounds.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <boost/optional/optional.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

class WorkingSet;

struct IndexScanParams {
    IndexScanParams(const IndexCatalogEntry* entry,
                    std::string indexName,
                    BSONObj keyPattern,
                    MultikeyPaths multikeyPaths,
                    bool multikey)
        : indexEntry(entry),
          name(std::move(indexName)),
          keyPattern(std::move(keyPattern)),
          multikeyPaths(std::move(multikeyPaths)),
          isMultiKey(multikey) {}

    IndexScanParams(OperationContext* opCtx,
                    const CollectionPtr& collection,
                    const IndexCatalogEntry* entry)
        : IndexScanParams(
              entry,
              entry->descriptor()->indexName(),
              entry->descriptor()->keyPattern(),
              [&]() {
                  MultikeyPaths paths;
                  collection->isIndexMultikey(opCtx, entry->descriptor()->indexName(), &paths);
                  return paths;
              }(),
              collection->isIndexMultikey(opCtx, entry->descriptor()->indexName(), nullptr)) {}

    const IndexCatalogEntry* indexEntry;

    std::string name;

    BSONObj keyPattern;

    MultikeyPaths multikeyPaths;

    bool isMultiKey;

    IndexBounds bounds;

    int direction{1};

    bool shouldDedup{false};

    // Do we want to add the key as metadata?
    bool addKeyMetadata{false};
};

/**
 * Stage scans over an index from startKey to endKey, returning results that pass the provided
 * filter.  Internally dedups on RecordId.
 *
 * Sub-stage preconditions: None.  Is a leaf and consumes no stage data.
 */
class IndexScan final : public RequiresIndexStage {
public:
    /**
     * Keeps track of what this index scan is currently doing so that it
     * can do the right thing on the next call to work().
     */
    enum ScanState {
        // Need to initialize the underlying index traversal machinery.
        INITIALIZING,

        // Skipping keys as directed by the _checker.
        NEED_SEEK,

        // Retrieving the next key, and applying the filter if necessary.
        GETTING_NEXT,

        // The index scan is finished.
        HIT_END
    };

    IndexScan(ExpressionContext* expCtx,
              CollectionAcquisition collection,
              IndexScanParams params,
              WorkingSet* workingSet,
              const MatchExpression* filter);

    StageState doWork(WorkingSetID* out) final;
    bool isEOF() const final;
    void doDetachFromOperationContext() final;
    void doReattachToOperationContext() final;

    StageType stageType() const final {
        return STAGE_IXSCAN;
    }

    std::unique_ptr<PlanStageStats> getStats() final;

    const IndexScanStats* getSpecificStats() const final {
        return &_specificStats;
    }

    static constexpr std::string_view kStageType = "IXSCAN"sv;

    const BSONObj& getKeyPattern() const {
        return _keyPattern;
    }

    bool isForward() const {
        return _forward;
    }

    const IndexBounds& getBounds() const {
        return _bounds;
    }

    const SimpleMemoryUsageTracker& getMemoryTracker_forTest() {
        return _memoryTracker;
    }

protected:
    void doSaveStateRequiresIndex() final;

    void doRestoreStateRequiresIndex() final;

private:
    /**
     * Initialize the underlying index Cursor, returning first result if any.
     */
    boost::optional<IndexKeyEntry> initIndexScan();

    // The WorkingSet we fill with results.  Not owned by us.
    WorkingSet* const _workingSet;

    std::unique_ptr<SortedDataInterface::Cursor> _indexCursor;
    const BSONObj _keyPattern;

    const IndexBounds _bounds;

    // Contains expressions only over fields in the index key.  We assume this is built
    // correctly by whomever creates this class.
    // The filter is not owned by us.
    const MatchExpression* const _filter;

    const int _direction;
    const bool _forward;

    // Do we want to add the key as metadata?
    const bool _addKeyMetadata;

    // Stats
    IndexScanStats _specificStats;

    // Keeps track of what work we need to do next.
    ScanState _scanState = ScanState::INITIALIZING;

    // True if we dedup on RecordId, false otherwise.
    const bool _dedup;

    // Which RecordIds have we returned?
    RecordIdDeduplicator _recordIdDeduplicator;

    //
    // This class employs one of two different algorithms for determining when the index scan
    // has reached the end:
    //

    //
    // 1) If the index scan is not a single contiguous interval, then we use an
    //    IndexBoundsChecker to determine which keys to return and when to stop scanning.
    //    In this case, _checker will be non-NULL.
    //

    std::unique_ptr<IndexBoundsChecker> _checker;
    IndexSeekPoint _seekPoint;

    //
    // 2) If the index scan is a single contiguous interval, then the scan can execute faster by
    //    letting the index cursor tell us when it hits the end, rather than repeatedly doing
    //    BSON compares against scanned keys. In this case _checker will be NULL.
    //

    // The key that the index cursor should start on/after.
    BSONObj _startKey;
    // The key that the index cursor should stop on/after.
    BSONObj _endKey;

    // Is the start key included in the range?
    bool _startKeyInclusive;
    // Is the end key included in the range?
    bool _endKeyInclusive;

    SimpleMemoryUsageTracker _memoryTracker;

    DeduplicatorReporter _dedupReporter;
};

}  // namespace mongo
