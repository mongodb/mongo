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

#include "mongo/db/exec/sort.h"

#include <algorithm>

#include "mongo/db/exec/working_set.h"
#include "mongo/db/exec/working_set_common.h"

namespace mongo {

    // Used in STL sort.
    struct WorkingSetComparison {
        WorkingSetComparison(WorkingSet* ws, BSONObj pattern) : _ws(ws), _pattern(pattern) { }

        bool operator()(const WorkingSetID& lhs, const WorkingSetID& rhs) const {
            WorkingSetMember* lhsMember = _ws->get(lhs);
            WorkingSetMember* rhsMember = _ws->get(rhs);

            BSONObjIterator it(_pattern);
            while (it.more()) {
                BSONElement patternElt = it.next();
                string fn = patternElt.fieldName();

                BSONElement lhsElt;
                verify(lhsMember->getFieldDotted(fn, &lhsElt));

                BSONElement rhsElt;
                verify(rhsMember->getFieldDotted(fn, &rhsElt));

                // false means don't compare field name.
                int x = lhsElt.woCompare(rhsElt, false);
                if (-1 == patternElt.number()) { x = -x; }
                if (x != 0) { return x < 0; }
            }

            // A comparator for use with sort is required to model a strict weak ordering, so
            // to satisfy irreflexivity we must return 'false' for elements that we consider
            // equivalent under the pattern.
            return false;
        }

        WorkingSet* _ws;
        BSONObj _pattern;
    };

    SortStage::SortStage(const SortStageParams& params, WorkingSet* ws, PlanStage* child)
        : _ws(ws), _child(child), _pattern(params.pattern), _sorted(false),
          _resultIterator(_data.end()) { }

    SortStage::~SortStage() { }

    bool SortStage::isEOF() {
        // We're done when our child has no more results, we've sorted the child's results, and
        // we've returned all sorted results.
        return _child->isEOF() && _sorted && (_data.end() == _resultIterator);
    }

    PlanStage::StageState SortStage::work(WorkingSetID* out) {
        ++_commonStats.works;

        if (isEOF()) { return PlanStage::IS_EOF; }

        // Still reading in results to sort.
        if (!_sorted) {
            WorkingSetID id;
            StageState code = _child->work(&id);

            if (PlanStage::ADVANCED == code) {
                // We let the data stay in the WorkingSet and sort using the IDs.
                _data.push_back(id);

                // Add it into the map for quick invalidation if it has a valid DiskLoc.
                // A DiskLoc may be invalidated at any time (during a yield).  We need to get into
                // the WorkingSet as quickly as possible to handle it.
                WorkingSetMember* member = _ws->get(id);
                if (member->hasLoc()) {
                    _wsidByDiskLoc[member->loc] = id;
                }

                ++_commonStats.needTime;
                return PlanStage::NEED_TIME;
            }
            else if (PlanStage::IS_EOF == code) {
                // TODO: We don't need the lock for this.  We could ask for a yield and do this work
                // unlocked.  Also, this is performing a lot of work for one call to work(...)
                std::sort(_data.begin(), _data.end(), WorkingSetComparison(_ws, _pattern));
                _resultIterator = _data.begin();
                _sorted = true;
                ++_commonStats.needTime;
                return PlanStage::NEED_TIME;
            }
            else {
                if (PlanStage::NEED_FETCH == code) {
                    ++_commonStats.needFetch;
                }
                else if (PlanStage::NEED_TIME == code) {
                    ++_commonStats.needTime;
                }
                return code;
            }
        }

        // Returning results.
        verify(_resultIterator != _data.end());
        verify(_sorted);
        *out = *_resultIterator++;
        ++_commonStats.advanced;
        return PlanStage::ADVANCED;
    }

    void SortStage::prepareToYield() {
        ++_commonStats.yields;
        _child->prepareToYield();
    }

    void SortStage::recoverFromYield() {
        ++_commonStats.unyields;
        _child->recoverFromYield();
    }

    void SortStage::invalidate(const DiskLoc& dl) {
        ++_commonStats.invalidates;
        _child->invalidate(dl);

        // _data contains indices into the WorkingSet, not actual data.  If a WorkingSetMember in
        // the WorkingSet needs to change state as a result of a DiskLoc invalidation, it will still
        // be at the same spot in the WorkingSet.  As such, we don't need to modify _data.

        DataMap::iterator it = _wsidByDiskLoc.find(dl);

        // If we're holding on to data that's got the DiskLoc we're invalidating...
        if (_wsidByDiskLoc.end() != it) {
            WorkingSetMember* member = _ws->get(it->second);
            WorkingSetCommon::fetchAndInvalidateLoc(member);
            _wsidByDiskLoc.erase(it);
            ++_specificStats.forcedFetches;
        }
    }

    PlanStageStats* SortStage::getStats() {
        _commonStats.isEOF = isEOF();

        auto_ptr<PlanStageStats> ret(new PlanStageStats(_commonStats));
        ret->setSpecific<SortStats>(_specificStats);
        ret->children.push_back(_child->getStats());
        return ret.release();
    }

}  // namespace mongo
