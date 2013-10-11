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

#include "mongo/db/exec/oplogstart.h"

#include "mongo/db/pdfile.h"
#include "mongo/db/storage/extent.h"

namespace mongo {

    // Does not take ownership.
    OplogStart::OplogStart(const string& ns, MatchExpression* filter, WorkingSet* ws)
        : _needInit(true),
          _backwardsScanning(false),
          _extentHopping(false),
          _done(false),
          _workingSet(ws),
          _ns(ns),
          _filter(filter) { }

    OplogStart::~OplogStart() { }

    PlanStage::StageState OplogStart::work(WorkingSetID* out) {
        // We do our (heavy) init in a work(), where work is expected.
        if (_needInit) {
            CollectionScanParams params;
            params.ns = _ns;
            params.direction = CollectionScanParams::BACKWARD;
            _cs.reset(new CollectionScan(params, _workingSet, NULL));
            _nsd = nsdetails(_ns.c_str());
            _needInit = false;
            _backwardsScanning = true;
            _timer.reset();
        }

        // How long will we look record by record backwards?
        static const int backwardsScanTime = 5;

        // If we're reading backwards, try again.
        if (_backwardsScanning) {
            // Still have time to succeed with reading backwards.
            if (_timer.seconds() < backwardsScanTime) {
                return workBackwardsScan(out);
            }
            switchToExtentHopping();
        }

        // Don't find it in time?  Swing from extent to extent like tarzan.com.
        verify(_extentHopping);
        return workExtentHopping(out);
    }

    PlanStage::StageState OplogStart::workExtentHopping(WorkingSetID* out) {
        if (_curloc.isNull()) {
            _done = true;
            return PlanStage::IS_EOF;
        }

        if (!_filter->matchesBSON(_curloc.obj())) {
            _done = true;
            WorkingSetID id = _workingSet->allocate();
            WorkingSetMember* member = _workingSet->get(id);
            member->loc = _curloc;
            member->obj = member->loc.obj();
            member->state = WorkingSetMember::LOC_AND_UNOWNED_OBJ;
            *out = id;
            return PlanStage::ADVANCED;
        }

        _curloc = prevExtentFirstLoc(_nsd, _curloc);
        return PlanStage::NEED_TIME;
    }

    void OplogStart::switchToExtentHopping() {
        // Transition from backwards scanning to extent hopping.
        _backwardsScanning = false;
        _extentHopping = true;

        // Toss the collection scan we were using.
        _cs.reset();

        // Set up our extent hopping state.  Get the start of the extent that we were collection
        // scanning.
        Extent* e = _curloc.rec()->myExtent(_curloc);
        if (!_nsd->capLooped() || (e->myLoc != _nsd->capExtent())) {
            _curloc = e->firstRecord;
        }
        else {
            // Direct quote:
            // Likely we are on the fresh side of capExtent, so return first fresh
            // record.  If we are on the stale side of capExtent, then the collection is
            // small and it doesn't matter if we start the extent scan with
            // capFirstNewRecord.
            _curloc = _nsd->capFirstNewRecord();
        }
    }

    PlanStage::StageState OplogStart::workBackwardsScan(WorkingSetID* out) {
        PlanStage::StageState state = _cs->work(out);

        // EOF.  Just start from the beginning, which is where we've hit.
        if (PlanStage::IS_EOF == state) {
            _done = true;
            return state;
        }

        if (PlanStage::ADVANCED != state) { return state; }

        WorkingSetMember* member = _workingSet->get(*out);
        verify(member->hasObj());
        verify(member->hasLoc());

        if (!_filter->matchesBSON(member->obj)) {
            _done = true;
            // DiskLoc is returned in *out.
            return PlanStage::ADVANCED;
        }
        else {
            _curloc = member->loc;
            _workingSet->free(*out);
            return PlanStage::NEED_TIME;
        }
    }

    bool OplogStart::isEOF() { return _done; }

    void OplogStart::invalidate(const DiskLoc& dl) {
        if (_needInit) { return; }
        if (_backwardsScanning) {
            _cs->invalidate(dl);
        }
        else {
            verify(_extentHopping);
            if (dl == _curloc) {
                _curloc = DiskLoc();
            }
        }
    }

    void OplogStart::prepareToYield() {
        if (_backwardsScanning) {
            _cs->prepareToYield();
        }
    }

    void OplogStart::recoverFromYield() {
        if (_backwardsScanning) {
            _cs->recoverFromYield();
        }
    }

    // static
    DiskLoc OplogStart::prevExtentFirstLoc(NamespaceDetails* nsd, const DiskLoc& rec ) {
        Extent *e = rec.rec()->myExtent( rec );
        if (nsd->capLooped() ) {
            while( true ) {
                // Advance e to preceding extent (looping to lastExtent if necessary).
                if ( e->xprev.isNull() ) {
                    e = nsd->lastExtent().ext();
                }
                else {
                    e = e->xprev.ext();
                }
                if ( e->myLoc == nsd->capExtent() ) {
                    // Reached the extent containing the oldest data in the collection.
                    return DiskLoc();
                }
                if ( !e->firstRecord.isNull() ) {
                    // Return the first record of the first non empty extent encountered.
                    return e->firstRecord;
                }
            }
        }
        else {
            while( true ) {
                if ( e->xprev.isNull() ) {
                    // Reached the beginning of the collection.
                    return DiskLoc();
                }
                e = e->xprev.ext();
                if ( !e->firstRecord.isNull() ) {
                    // Return the first record of the first non empty extent encountered.
                    return e->firstRecord;
                }
            }
        }
    }

}  // namespace mongo
