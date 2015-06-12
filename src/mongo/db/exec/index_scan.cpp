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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/exec/index_scan.h"

#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/exec/filter.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set_computed_data.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/util/log.h"

namespace {

    // Return a value in the set {-1, 0, 1} to represent the sign of parameter i.
    int sgn(int i) {
        if (i == 0)
            return 0;
        return i > 0 ? 1 : -1;
    }

}  // namespace

namespace mongo {

    // static
    const char* IndexScan::kStageType = "IXSCAN";

    IndexScan::IndexScan(OperationContext* txn,
                         const IndexScanParams& params,
                         WorkingSet* workingSet,
                         const MatchExpression* filter)
        : _txn(txn),
          _workingSet(workingSet),
          _iam(params.descriptor->getIndexCatalog()->getIndex(params.descriptor)),
          _keyPattern(params.descriptor->keyPattern().getOwned()),
          _scanState(INITIALIZING),
          _filter(filter),
          _shouldDedup(true),
          _forward(params.direction == 1),
          _params(params),
          _commonStats(kStageType),
          _endKeyInclusive(false) {

        // We can't always access the descriptor in the call to getStats() so we pull
        // any info we need for stats reporting out here.
        _specificStats.keyPattern = _keyPattern;
        _specificStats.indexName = _params.descriptor->indexName();
        _specificStats.isMultiKey = _params.descriptor->isMultikey(_txn);
        _specificStats.isUnique = _params.descriptor->unique();
        _specificStats.isSparse = _params.descriptor->isSparse();
        _specificStats.isPartial = _params.descriptor->isPartial();
        _specificStats.isTTL = _params.descriptor->isTTL();
        _specificStats.expireAfterSeconds = _params.descriptor->expireAfterSeconds();
        _specificStats.indexVersion = _params.descriptor->version();
    }

    boost::optional<IndexKeyEntry> IndexScan::initIndexScan() {
        if (_params.doNotDedup) {
            _shouldDedup = false;
        }
        else {
            // TODO it is incorrect to rely on this not changing. SERVER-17678
            _shouldDedup = _params.descriptor->isMultikey(_txn);
        }

        // Perform the possibly heavy-duty initialization of the underlying index cursor.
        _indexCursor = _iam->newCursor(_txn, _forward);

        if (_params.bounds.isSimpleRange) {
            // Start at one key, end at another.
            _endKey = _params.bounds.endKey;
            _endKeyInclusive = _params.bounds.endKeyInclusive;
            _indexCursor->setEndPosition(_endKey, _endKeyInclusive);
            return _indexCursor->seek(_params.bounds.startKey, /*inclusive*/true);
        }
        else {
            // For single intervals, we can use an optimized scan which checks against the position
            // of an end cursor.  For all other index scans, we fall back on using
            // IndexBoundsChecker to determine when we've finished the scan.
            BSONObj startKey;
            bool startKeyInclusive;
            if (IndexBoundsBuilder::isSingleInterval(_params.bounds,
                                                     &startKey,
                                                     &startKeyInclusive,
                                                     &_endKey,
                                                     &_endKeyInclusive)) {

                _indexCursor->setEndPosition(_endKey, _endKeyInclusive);
                return _indexCursor->seek(startKey, startKeyInclusive);
            }
            else {
                _checker.reset(new IndexBoundsChecker(&_params.bounds,
                                                      _keyPattern,
                                                      _params.direction));

                if (!_checker->getStartSeekPoint(&_seekPoint))
                    return boost::none;

                return _indexCursor->seek(_seekPoint);
            }
        }
    }

    PlanStage::StageState IndexScan::work(WorkingSetID* out) {
        ++_commonStats.works;

        // Adds the amount of time taken by work() to executionTimeMillis.
        ScopedTimer timer(&_commonStats.executionTimeMillis);

        // Get the next kv pair from the index, if any.
        boost::optional<IndexKeyEntry> kv;
        try {
            switch (_scanState) {
            case INITIALIZING: kv = initIndexScan(); break;
            case GETTING_NEXT: kv = _indexCursor->next(); break;
            case NEED_SEEK: kv = _indexCursor->seek(_seekPoint); break;
            case HIT_END: return PlanStage::IS_EOF;
            }
        }
        catch (const WriteConflictException& wce) {
            *out = WorkingSet::INVALID_ID;
            return PlanStage::NEED_YIELD;
        }

        if (kv) {
            // In debug mode, check that the cursor isn't lying to us.
            if (kDebugBuild && !_endKey.isEmpty()) {
                int cmp = kv->key.woCompare(_endKey,
                                            Ordering::make(_params.descriptor->keyPattern()),
                                            /*compareFieldNames*/false);
                if (cmp == 0) dassert(_endKeyInclusive);
                dassert(_forward ? cmp <= 0 : cmp >= 0);
            }

            ++_specificStats.keysExamined;
            if (_params.maxScan && _specificStats.keysExamined >= _params.maxScan) {
                kv = boost::none;
            }
        }

        if (kv && _checker) {
            switch (_checker->checkKey(kv->key, &_seekPoint)) {
            case IndexBoundsChecker::VALID:
                break;

            case IndexBoundsChecker::DONE:
                kv = boost::none;
                break;

            case IndexBoundsChecker::MUST_ADVANCE:
                _scanState = NEED_SEEK;
                _commonStats.needTime++;
                return PlanStage::NEED_TIME;
            }
        }

        if (!kv) {
            _scanState = HIT_END;
            _commonStats.isEOF = true;
            _indexCursor.reset();
            return PlanStage::IS_EOF;
        }

        _scanState = GETTING_NEXT;

        if (_shouldDedup) {
            ++_specificStats.dupsTested;
            if (!_returned.insert(kv->loc).second) {
                // We've seen this RecordId before. Skip it this time.
                ++_specificStats.dupsDropped;
                ++_commonStats.needTime;
                return PlanStage::NEED_TIME;
            }
        }

        if (_filter) {
            if (!Filter::passes(kv->key, _keyPattern, _filter)) {
                ++_commonStats.needTime;
                return PlanStage::NEED_TIME;
            }
        }

        if (!kv->key.isOwned()) kv->key = kv->key.getOwned();

        // We found something to return, so fill out the WSM.
        WorkingSetID id = _workingSet->allocate();
        WorkingSetMember* member = _workingSet->get(id);
        member->loc = kv->loc;
        member->keyData.push_back(IndexKeyDatum(_keyPattern, kv->key, _iam));
        member->state = WorkingSetMember::LOC_AND_IDX;

        if (_params.addKeyMetadata) {
            BSONObjBuilder bob;
            bob.appendKeys(_keyPattern, kv->key);
            member->addComputed(new IndexKeyComputedData(bob.obj()));
        }

        *out = id;
        ++_commonStats.advanced;
        return PlanStage::ADVANCED;
    }

    bool IndexScan::isEOF() {
        return _commonStats.isEOF;
    }

    void IndexScan::saveState() {
        if (!_txn) {
            // We were already saved. Nothing to do.
            return;
        }

        _txn = NULL;
        ++_commonStats.yields;
        if (!_indexCursor) return;

        if (_scanState == NEED_SEEK) {
            _indexCursor->saveUnpositioned();
            return;
        }

        _indexCursor->savePositioned();
    }

    void IndexScan::restoreState(OperationContext* opCtx) {
        invariant(_txn == NULL);
        _txn = opCtx;
        ++_commonStats.unyields;

        if (_indexCursor) _indexCursor->restore(opCtx);
    }

    void IndexScan::invalidate(OperationContext* txn, const RecordId& dl, InvalidationType type) {
        ++_commonStats.invalidates;

        // The only state we're responsible for holding is what RecordIds to drop.  If a document
        // mutates the underlying index cursor will deal with it.
        if (INVALIDATION_MUTATION == type) {
            return;
        }

        // If we see this RecordId again, it may not be the same document it was before, so we want
        // to return it if we see it again.
        unordered_set<RecordId, RecordId::Hasher>::iterator it = _returned.find(dl);
        if (it != _returned.end()) {
            ++_specificStats.seenInvalidated;
            _returned.erase(it);
        }
    }

    std::vector<PlanStage*> IndexScan::getChildren() const {
        return {};
    }

    PlanStageStats* IndexScan::getStats() {
        // WARNING: this could be called even if the collection was dropped.  Do not access any
        // catalog information here.

        // Add a BSON representation of the filter to the stats tree, if there is one.
        if (NULL != _filter) {
            BSONObjBuilder bob;
            _filter->toBSON(&bob);
            _commonStats.filter = bob.obj();
        }

        // These specific stats fields never change.
        if (_specificStats.indexType.empty()) {
            _specificStats.indexType = "BtreeCursor"; // TODO amName;

            _specificStats.indexBounds = _params.bounds.toBSON();

            _specificStats.direction = _params.direction;
        }

        std::unique_ptr<PlanStageStats> ret(new PlanStageStats(_commonStats, STAGE_IXSCAN));
        ret->specific.reset(new IndexScanStats(_specificStats));
        return ret.release();
    }

    const CommonStats* IndexScan::getCommonStats() const {
        return &_commonStats;
    }

    const SpecificStats* IndexScan::getSpecificStats() const {
        return &_specificStats;
    }

}  // namespace mongo
