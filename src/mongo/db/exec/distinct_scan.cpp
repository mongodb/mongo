/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/db/exec/distinct_scan.h"

#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/exec/filter.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set_computed_data.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"

namespace mongo {

using std::unique_ptr;
using std::vector;

// static
const char* DistinctScan::kStageType = "DISTINCT_SCAN";

DistinctScan::DistinctScan(OperationContext* txn,
                           const DistinctParams& params,
                           WorkingSet* workingSet)
    : _txn(txn),
      _workingSet(workingSet),
      _descriptor(params.descriptor),
      _iam(params.descriptor->getIndexCatalog()->getIndex(params.descriptor)),
      _params(params),
      _checker(&_params.bounds, _descriptor->keyPattern(), _params.direction),
      _commonStats(kStageType) {
    _specificStats.keyPattern = _params.descriptor->keyPattern();
    _specificStats.indexName = _params.descriptor->indexName();
    _specificStats.indexVersion = _params.descriptor->version();

    // Set up our initial seek. If there is no valid data, just mark as EOF.
    _commonStats.isEOF = !_checker.getStartSeekPoint(&_seekPoint);
}

PlanStage::StageState DistinctScan::work(WorkingSetID* out) {
    ++_commonStats.works;
    if (_commonStats.isEOF)
        return PlanStage::IS_EOF;

    // Adds the amount of time taken by work() to executionTimeMillis.
    ScopedTimer timer(&_commonStats.executionTimeMillis);

    boost::optional<IndexKeyEntry> kv;
    try {
        if (!_cursor)
            _cursor = _iam->newCursor(_txn, _params.direction == 1);
        kv = _cursor->seek(_seekPoint);
    } catch (const WriteConflictException& wce) {
        *out = WorkingSet::INVALID_ID;
        return PlanStage::NEED_YIELD;
    }

    if (!kv) {
        _commonStats.isEOF = true;
        return PlanStage::IS_EOF;
    }

    ++_specificStats.keysExamined;

    switch (_checker.checkKey(kv->key, &_seekPoint)) {
        case IndexBoundsChecker::MUST_ADVANCE:
            // Try again next time. The checker has adjusted the _seekPoint.
            ++_commonStats.needTime;
            return PlanStage::NEED_TIME;

        case IndexBoundsChecker::DONE:
            // There won't be a next time.
            _commonStats.isEOF = true;
            _cursor.reset();
            return IS_EOF;

        case IndexBoundsChecker::VALID:
            // Return this key. Adjust the _seekPoint so that it is exclusive on the field we
            // are using.

            if (!kv->key.isOwned())
                kv->key = kv->key.getOwned();
            _seekPoint.keyPrefix = kv->key;
            _seekPoint.prefixLen = _params.fieldNo + 1;
            _seekPoint.prefixExclusive = true;

            // Package up the result for the caller.
            WorkingSetID id = _workingSet->allocate();
            WorkingSetMember* member = _workingSet->get(id);
            member->loc = kv->loc;
            member->keyData.push_back(IndexKeyDatum(_descriptor->keyPattern(), kv->key, _iam));
            member->state = WorkingSetMember::LOC_AND_IDX;

            *out = id;
            ++_commonStats.advanced;
            return PlanStage::ADVANCED;
    }
    invariant(false);
}

bool DistinctScan::isEOF() {
    return _commonStats.isEOF;
}

void DistinctScan::saveState() {
    _txn = NULL;
    ++_commonStats.yields;

    // We always seek, so we don't care where the cursor is.
    if (_cursor)
        _cursor->saveUnpositioned();
}

void DistinctScan::restoreState(OperationContext* opCtx) {
    invariant(_txn == NULL);
    _txn = opCtx;
    ++_commonStats.unyields;

    if (_cursor)
        _cursor->restore(opCtx);
}

void DistinctScan::invalidate(OperationContext* txn, const RecordId& dl, InvalidationType type) {
    ++_commonStats.invalidates;
}

vector<PlanStage*> DistinctScan::getChildren() const {
    vector<PlanStage*> empty;
    return empty;
}

PlanStageStats* DistinctScan::getStats() {
    unique_ptr<PlanStageStats> ret(new PlanStageStats(_commonStats, STAGE_DISTINCT_SCAN));
    ret->specific.reset(new DistinctScanStats(_specificStats));
    return ret.release();
}

const CommonStats* DistinctScan::getCommonStats() const {
    return &_commonStats;
}

const SpecificStats* DistinctScan::getSpecificStats() const {
    return &_specificStats;
}

}  // namespace mongo
