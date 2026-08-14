// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/classic/orphan_chunk_skipper.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/requires_index_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/shard_filterer_impl.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/compiler/physical_model/index_bounds/index_bounds.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace mongo {
using namespace std::literals::string_view_literals;

class IndexAccessMethod;
class IndexDescriptor;
class WorkingSet;

struct DistinctParams {
    DistinctParams(const IndexCatalogEntry* entry,
                   std::string indexName,
                   BSONObj keyPattern,
                   MultikeyPaths multikeyPaths,
                   bool multikey)
        : indexEntry(entry),
          name(std::move(indexName)),
          keyPattern(std::move(keyPattern)),
          multikeyPaths(std::move(multikeyPaths)),
          isMultiKey(multikey) {
        tassert(11051642, "Expecting Index Entry.", indexEntry);
    }

    DistinctParams(OperationContext* opCtx,
                   const CollectionPtr& collection,
                   const IndexCatalogEntry* entry)
        : DistinctParams(
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

    int scanDirection{1};

    // What are the bounds?
    IndexBounds bounds;

    // What field in the index's key pattern is the one we're distinct-ing over?
    // For example:
    // If we have an index {a:1, b:1} we could use it to distinct over either 'a' or 'b'.
    // If we distinct over 'a' the position is 0.
    // If we distinct over 'b' the position is 1.
    int fieldNo{0};
};

/**
 * Executes an index scan over the provided bounds. However, rather than looking at every key in the
 * bounds, it skips to the next value of the _params.fieldNo-th indexed field. This is because
 * distinct only cares about distinct values for that field, so there is no point in examining all
 * keys with the same value for that field.
 */
class DistinctScan final : public RequiresIndexStage {
public:
    DistinctScan(ExpressionContext* expCtx,
                 CollectionAcquisition collection,
                 DistinctParams params,
                 WorkingSet* workingSet,
                 std::unique_ptr<ShardFiltererImpl> _shardFilterer = nullptr,
                 bool needsFetch = false);

    StageState doWork(WorkingSetID* out) final;
    bool isEOF() const final;
    void doDetachFromOperationContext() final;
    void doReattachToOperationContext() final;

    StageType stageType() const final {
        return STAGE_DISTINCT_SCAN;
    }

    std::unique_ptr<PlanStageStats> getStats() final;

    const SpecificStats* getSpecificStats() const final;

    static constexpr std::string_view kStageType = "DISTINCT_SCAN"sv;

protected:
    void doSaveStateRequiresIndex() final;

    void doRestoreStateRequiresIndex() final;

private:
    // Helper method containing logic for fetching.
    PlanStage::StageState doFetch(WorkingSetMember* member, WorkingSetID id, WorkingSetID* out);

    // The WorkingSet we annotate with results.  Not owned by us.
    WorkingSet* _workingSet;

    const BSONObj _keyPattern;

    const int _scanDirection = 1;

    const IndexBounds _bounds;

    const size_t _fieldNo = 0;

    // The cursor we use to navigate the tree.
    std::unique_ptr<SortedDataInterface::Cursor> _cursor;

    // _checker gives us our start key and ensures we stay in bounds.
    IndexBoundsChecker _checker;
    IndexSeekPoint _seekPoint;

    // State used to implement shard filtering.
    std::unique_ptr<ShardFiltererImpl> _shardFilterer;
    boost::optional<OrphanChunkSkipper> _chunkSkipper;
    // When '_needsSequentialScan' is true, the 'DistinctScan' stage cannot skip over consecutive
    // index entries with the same value, as it normally would, but must instead examine the next
    // entry, as a normal index scan does. This step is necessary when the '_shardFilterer' rejects
    // an entry, requiring the DistinctScan to examine other entries with the same value to
    // determine if one of them may be accepted.
    bool _needsSequentialScan = false;
    // When set to true, performs a fetch before outputting.
    const bool _needsFetch;
    // The cursor we use to fetch.
    std::unique_ptr<SeekableRecordCursor> _fetchCursor;
    // In case of a yield while fetching, this is the id of the working set entry that we need to
    // retry fetching.
    WorkingSetID _idRetrying = WorkingSet::INVALID_ID;

    // Stats
    DistinctScanStats _specificStats;
};

}  // namespace mongo
