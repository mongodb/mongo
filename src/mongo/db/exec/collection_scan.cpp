/**
 *    Copyright (C) 2013 10gen Inc.
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
 */

#include "mongo/db/exec/collection_scan.h"

#include "mongo/db/exec/collection_scan_common.h"
#include "mongo/db/exec/collection_iterator.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/namespace_details.h"
#include "mongo/db/pdfile.h"

namespace mongo {

    CollectionScan::CollectionScan(const CollectionScanParams& params,
                                   WorkingSet* workingSet,
                                   Matcher* matcher) : _workingSet(workingSet),
                                                       _matcher(matcher),
                                                       _params(params),
                                                       _nsDropped(false) { }

    PlanStage::StageState CollectionScan::work(WorkingSetID* out) {
        ++_commonStats.works;

        if (NULL == _iter) {
            NamespaceDetails* nsd = nsdetails(_params.ns);

            if (NULL == nsd) {
                _nsDropped = true;
                return PlanStage::FAILURE;
            }

            if (nsd->isCapped()) {
                _iter.reset(new CappedIterator(_params.ns, _params.start, _params.tailable,
                                               _params.direction));
            }
            else {
                _iter.reset(new FlatIterator(_params.ns, _params.start, _params.direction));
            }

            ++_commonStats.needTime;
            return PlanStage::NEED_TIME;
        }

        if (isEOF()) { return PlanStage::IS_EOF; }

        WorkingSetID id = _workingSet->allocate();
        WorkingSetMember* member = _workingSet->get(id);
        member->loc = _iter->getNext();;
        member->obj = member->loc.obj();
        member->state = WorkingSetMember::LOC_AND_UNOWNED_OBJ;

        if (NULL == _matcher || _matcher->matches(member)) {
            *out = id;
            ++_commonStats.advanced;
            return PlanStage::ADVANCED;
        }
        else {
            _workingSet->free(id);
            ++_commonStats.needTime;
            return PlanStage::NEED_TIME;
        }
    }

    bool CollectionScan::isEOF() {
        if (_nsDropped) { return true; }
        if (NULL == _iter) { return false; }
        return _iter->isEOF();
    }

    void CollectionScan::invalidate(const DiskLoc& dl) {
        ++_commonStats.yields;
        _iter->invalidate(dl);
    }

    void CollectionScan::prepareToYield() {
        ++_commonStats.unyields;
        _iter->prepareToYield();
    }

    void CollectionScan::recoverFromYield() {
        ++_commonStats.invalidates;
        if (!_iter->recoverFromYield()) {
            _nsDropped = true;
        }
    }

    PlanStageStats* CollectionScan::getStats() {
        _commonStats.isEOF = isEOF();
        return new PlanStageStats(_commonStats);
    }

}  // namespace mongo
