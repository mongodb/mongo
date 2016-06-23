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

#include "mongo/db/exec/count_scan.h"

#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/stdx/memory.h"

namespace mongo {

namespace {
/**
 * This function replaces field names in *replace* with those from the object
 * *fieldNames*, preserving field ordering.  Both objects must have the same
 * number of fields.
 *
 * Example:
 *
 *     replaceBSONKeyNames({ 'a': 1, 'b' : 1 }, { '': 'foo', '', 'bar' }) =>
 *
 *         { 'a' : 'foo' }, { 'b' : 'bar' }
 */
BSONObj replaceBSONFieldNames(const BSONObj& replace, const BSONObj& fieldNames) {
    invariant(replace.nFields() == fieldNames.nFields());

    BSONObjBuilder bob;
    BSONObjIterator iter = fieldNames.begin();

    for (const BSONElement& el : replace) {
        bob.appendAs(el, (*iter++).fieldNameStringData());
    }

    return bob.obj();
}
}

using std::unique_ptr;
using std::vector;
using stdx::make_unique;

// static
const char* CountScan::kStageType = "COUNT_SCAN";

CountScan::CountScan(OperationContext* txn, const CountScanParams& params, WorkingSet* workingSet)
    : PlanStage(kStageType, txn),
      _workingSet(workingSet),
      _descriptor(params.descriptor),
      _iam(params.descriptor->getIndexCatalog()->getIndex(params.descriptor)),
      _shouldDedup(params.descriptor->isMultikey(txn)),
      _params(params) {
    _specificStats.keyPattern = _params.descriptor->keyPattern();
    if (BSONElement collationElement = _params.descriptor->getInfoElement("collation")) {
        invariant(collationElement.isABSONObj());
        _specificStats.collation = collationElement.Obj().getOwned();
    }
    _specificStats.indexName = _params.descriptor->indexName();
    _specificStats.isMultiKey = _params.descriptor->isMultikey(txn);
    _specificStats.multiKeyPaths = _params.descriptor->getMultikeyPaths(txn);
    _specificStats.isUnique = _params.descriptor->unique();
    _specificStats.isSparse = _params.descriptor->isSparse();
    _specificStats.isPartial = _params.descriptor->isPartial();
    _specificStats.indexVersion = _params.descriptor->version();

    // endKey must be after startKey in index order since we only do forward scans.
    dassert(_params.startKey.woCompare(_params.endKey,
                                       Ordering::make(params.descriptor->keyPattern()),
                                       /*compareFieldNames*/ false) <= 0);
}


PlanStage::StageState CountScan::doWork(WorkingSetID* out) {
    if (_commonStats.isEOF)
        return PlanStage::IS_EOF;

    boost::optional<IndexKeyEntry> entry;
    const bool needInit = !_cursor;
    try {
        // We don't care about the keys.
        const auto kWantLoc = SortedDataInterface::Cursor::kWantLoc;

        if (needInit) {
            // First call to work().  Perform cursor init.
            _cursor = _iam->newCursor(getOpCtx());
            _cursor->setEndPosition(_params.endKey, _params.endKeyInclusive);

            entry = _cursor->seek(_params.startKey, _params.startKeyInclusive, kWantLoc);
        } else {
            entry = _cursor->next(kWantLoc);
        }
    } catch (const WriteConflictException& wce) {
        if (needInit) {
            // Release our cursor and try again next time.
            _cursor.reset();
        }
        *out = WorkingSet::INVALID_ID;
        return PlanStage::NEED_YIELD;
    }

    ++_specificStats.keysExamined;

    if (!entry) {
        _commonStats.isEOF = true;
        _cursor.reset();
        return PlanStage::IS_EOF;
    }

    if (_shouldDedup && !_returned.insert(entry->loc).second) {
        // *loc was already in _returned.
        return PlanStage::NEED_TIME;
    }

    WorkingSetID id = _workingSet->allocate();
    _workingSet->transitionToRecordIdAndObj(id);
    *out = id;
    return PlanStage::ADVANCED;
}

bool CountScan::isEOF() {
    return _commonStats.isEOF;
}

void CountScan::doSaveState() {
    if (_cursor)
        _cursor->save();
}

void CountScan::doRestoreState() {
    if (_cursor)
        _cursor->restore();

    // This can change during yielding.
    // TODO this isn't sufficient. See SERVER-17678.
    _shouldDedup = _descriptor->isMultikey(getOpCtx());
}

void CountScan::doDetachFromOperationContext() {
    if (_cursor)
        _cursor->detachFromOperationContext();
}

void CountScan::doReattachToOperationContext() {
    if (_cursor)
        _cursor->reattachToOperationContext(getOpCtx());
}

void CountScan::doInvalidate(OperationContext* txn, const RecordId& dl, InvalidationType type) {
    // The only state we're responsible for holding is what RecordIds to drop.  If a document
    // mutates the underlying index cursor will deal with it.
    if (INVALIDATION_MUTATION == type) {
        return;
    }

    // If we see this RecordId again, it may not be the same document it was before, so we want
    // to return it if we see it again.
    unordered_set<RecordId, RecordId::Hasher>::iterator it = _returned.find(dl);
    if (it != _returned.end()) {
        _returned.erase(it);
    }
}

unique_ptr<PlanStageStats> CountScan::getStats() {
    unique_ptr<PlanStageStats> ret = make_unique<PlanStageStats>(_commonStats, STAGE_COUNT_SCAN);

    unique_ptr<CountScanStats> countStats = make_unique<CountScanStats>(_specificStats);
    countStats->keyPattern = _specificStats.keyPattern.getOwned();

    countStats->startKey = replaceBSONFieldNames(_params.startKey, countStats->keyPattern);
    countStats->startKeyInclusive = _params.startKeyInclusive;
    countStats->endKey = replaceBSONFieldNames(_params.endKey, countStats->keyPattern);
    countStats->endKeyInclusive = _params.endKeyInclusive;

    ret->specific = std::move(countStats);

    return ret;
}

const SpecificStats* CountScan::getSpecificStats() const {
    return &_specificStats;
}

}  // namespace mongo
