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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/classic/orphan_chunk_skipper.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/requires_index_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/shard_filterer_impl.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/index_catalog_entry.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/compiler/physical_model/index_bounds/index_bounds.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <string>
#include <utility>

namespace mongo {

class IndexAccessMethod;
class IndexDescriptor;
class WorkingSet;

struct DistinctParams {
    DistinctParams(const IndexDescriptor* descriptor,
                   std::string indexName,
                   BSONObj keyPattern,
                   MultikeyPaths multikeyPaths,
                   bool multikey)
        : indexDescriptor(descriptor),
          name(std::move(indexName)),
          keyPattern(std::move(keyPattern)),
          multikeyPaths(std::move(multikeyPaths)),
          isMultiKey(multikey) {
        invariant(indexDescriptor);
    }

    DistinctParams(OperationContext* opCtx,
                   const CollectionPtr& collection,
                   const IndexDescriptor* descriptor)
        : DistinctParams(descriptor,
                         descriptor->indexName(),
                         descriptor->keyPattern(),
                         descriptor->getEntry()->getMultikeyPaths(opCtx, collection),
                         descriptor->getEntry()->isMultikey(opCtx, collection)) {}

    const IndexDescriptor* indexDescriptor;
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
                 VariantCollectionPtrOrAcquisition collection,
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

    static const char* kStageType;

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
