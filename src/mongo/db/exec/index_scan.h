/**
 *    Copyright (C) 2013-2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once


#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/index_bounds.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/platform/unordered_set.h"

namespace mongo {

class IndexAccessMethod;
class IndexDescriptor;
class WorkingSet;

struct IndexScanParams {
    IndexScanParams()
        : descriptor(NULL), direction(1), doNotDedup(false), maxScan(0), addKeyMetadata(false) {}

    const IndexDescriptor* descriptor;

    IndexBounds bounds;

    int direction;

    bool doNotDedup;

    // How many keys will we look at?
    size_t maxScan;

    // Do we want to add the key as metadata?
    bool addKeyMetadata;
};

/**
 * Stage scans over an index from startKey to endKey, returning results that pass the provided
 * filter.  Internally dedups on RecordId.
 *
 * Sub-stage preconditions: None.  Is a leaf and consumes no stage data.
 */
class IndexScan final : public PlanStage {
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

    IndexScan(OperationContext* txn,
              const IndexScanParams& params,
              WorkingSet* workingSet,
              const MatchExpression* filter);

    StageState doWork(WorkingSetID* out) final;
    bool isEOF() final;
    void doSaveState() final;
    void doRestoreState() final;
    void doDetachFromOperationContext() final;
    void doReattachToOperationContext() final;
    void doInvalidate(OperationContext* txn, const RecordId& dl, InvalidationType type) final;

    StageType stageType() const final {
        return STAGE_IXSCAN;
    }

    std::unique_ptr<PlanStageStats> getStats() final;

    const SpecificStats* getSpecificStats() const final;

    static const char* kStageType;

private:
    /**
     * Initialize the underlying index Cursor, returning first result if any.
     */
    boost::optional<IndexKeyEntry> initIndexScan();

    // The WorkingSet we fill with results.  Not owned by us.
    WorkingSet* const _workingSet;

    // Index access.
    const IndexAccessMethod* const _iam;  // owned by Collection -> IndexCatalog
    std::unique_ptr<SortedDataInterface::Cursor> _indexCursor;
    const BSONObj _keyPattern;

    // Keeps track of what work we need to do next.
    ScanState _scanState;

    // Contains expressions only over fields in the index key.  We assume this is built
    // correctly by whomever creates this class.
    // The filter is not owned by us.
    const MatchExpression* const _filter;

    // Could our index have duplicates?  If so, we use _returned to dedup.
    bool _shouldDedup;
    unordered_set<RecordId, RecordId::Hasher> _returned;

    const bool _forward;
    const IndexScanParams _params;

    // Stats
    IndexScanStats _specificStats;

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

    // The key that the index cursor should stop on/after.
    BSONObj _endKey;

    // Is the end key included in the range?
    bool _endKeyInclusive;
};

}  // namespace mongo
