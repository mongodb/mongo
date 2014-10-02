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

#include "mongo/db/exec/and_hash.h"

#include "mongo/db/exec/and_common-inl.h"
#include "mongo/db/exec/filter.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/util/mongoutils/str.h"

namespace {

    // Upper limit for buffered data.
    // Stage execution will fail once size of all buffered data exceeds this threshold.
    const size_t kDefaultMaxMemUsageBytes = 32 * 1024 * 1024;

} // namespace

namespace mongo {

    using std::auto_ptr;

    const size_t AndHashStage::kLookAheadWorks = 10;

    // static
    const char* AndHashStage::kStageType = "AND_HASH";

    AndHashStage::AndHashStage(OperationContext* txn,
                               WorkingSet* ws, 
                               const MatchExpression* filter,
                               const Collection* collection)
        : _txn(txn),
          _collection(collection),
          _ws(ws),
          _filter(filter),
          _hashingChildren(true),
          _currentChild(0),
          _commonStats(kStageType),
          _memUsage(0),
          _maxMemUsage(kDefaultMaxMemUsageBytes) {}

    AndHashStage::AndHashStage(OperationContext* txn,
                               WorkingSet* ws, 
                               const MatchExpression* filter,
                               const Collection* collection,
                               size_t maxMemUsage)
        : _txn(txn),
          _collection(collection),
          _ws(ws),
          _filter(filter),
          _hashingChildren(true),
          _currentChild(0),
          _commonStats(kStageType),
          _memUsage(0),
          _maxMemUsage(maxMemUsage) {}

    AndHashStage::~AndHashStage() {
        for (size_t i = 0; i < _children.size(); ++i) { delete _children[i]; }
    }

    void AndHashStage::addChild(PlanStage* child) { _children.push_back(child); }

    size_t AndHashStage::getMemUsage() const {
        return _memUsage;
    }

    bool AndHashStage::isEOF() {
        // This is empty before calling work() and not-empty after.
        if (_lookAheadResults.empty()) { return false; }

        // Either we're busy hashing children, in which case we're not done yet.
        if (_hashingChildren) { return false; }

        // Or we're streaming in results from the last child.

        // If there's nothing to probe against, we're EOF.
        if (_dataMap.empty()) { return true; }

        // Otherwise, we're done when the last child is done.
        invariant(_children.size() >= 2);
        return (WorkingSet::INVALID_ID == _lookAheadResults[_children.size() - 1])
               && _children[_children.size() - 1]->isEOF();
    }

    PlanStage::StageState AndHashStage::work(WorkingSetID* out) {
        ++_commonStats.works;

        // Adds the amount of time taken by work() to executionTimeMillis.
        ScopedTimer timer(&_commonStats.executionTimeMillis);

        if (isEOF()) { return PlanStage::IS_EOF; }

        // Fast-path for one of our children being EOF immediately.  We work each child a few times.
        // If it hits EOF, the AND cannot output anything.  If it produces a result, we stash that
        // result in _lookAheadResults.
        if (_lookAheadResults.empty()) {
            // INVALID_ID means that the child didn't produce a valid result.

            // We specifically are not using .resize(size, value) here because C++11 builds don't
            // seem to resolve WorkingSet::INVALID_ID during linking.
            _lookAheadResults.resize(_children.size());
            for (size_t i = 0; i < _children.size(); ++i) {
                _lookAheadResults[i] =  WorkingSet::INVALID_ID;
            }

            // Work each child some number of times until it's either EOF or produces
            // a result.  If it's EOF this whole stage will be EOF.  If it produces a
            // result we cache it for later.
            for (size_t i = 0; i < _children.size(); ++i) {
                PlanStage* child = _children[i];
                for (size_t j = 0; j < kLookAheadWorks; ++j) {
                    StageState childStatus = child->work(&_lookAheadResults[i]);

                    if (PlanStage::IS_EOF == childStatus || PlanStage::DEAD == childStatus) {

                        // A child went right to EOF.  Bail out.
                        _hashingChildren = false;
                        _dataMap.clear();
                        return PlanStage::IS_EOF;
                    }
                    else if (PlanStage::ADVANCED == childStatus) {
                        // We have a result cached in _lookAheadResults[i].  Stop looking at this
                        // child.
                        break;
                    }
                    else if (PlanStage::FAILURE == childStatus) {
                        // Propage error to parent.
                        *out = _lookAheadResults[i];
                        // If a stage fails, it may create a status WSM to indicate why it
                        // failed, in which case 'id' is valid.  If ID is invalid, we
                        // create our own error message.
                        if (WorkingSet::INVALID_ID == *out) {
                            mongoutils::str::stream ss;
                            ss << "hashed AND stage failed to read in look ahead results "
                               << "from child " << i;
                            Status status(ErrorCodes::InternalError, ss);
                            *out = WorkingSetCommon::allocateStatusMember( _ws, status);
                        }

                        _hashingChildren = false;
                        _dataMap.clear();
                        return PlanStage::FAILURE;
                    }
                    // We ignore NEED_TIME.
                }
            }

            // We did a bunch of work above, return NEED_TIME to be fair.
            return PlanStage::NEED_TIME;
        }

        // An AND is either reading the first child into the hash table, probing against the hash
        // table with subsequent children, or checking the last child's results to see if they're
        // in the hash table.

        // We read the first child into our hash table.
        if (_hashingChildren) {
            // Check memory usage of previously hashed results.
            if (_memUsage > _maxMemUsage) {
                mongoutils::str::stream ss;
                ss << "hashed AND stage buffered data usage of " << _memUsage
                   << " bytes exceeds internal limit of " << kDefaultMaxMemUsageBytes << " bytes";
                Status status(ErrorCodes::Overflow, ss);
                *out = WorkingSetCommon::allocateStatusMember( _ws, status);
                return PlanStage::FAILURE;
            }

            if (0 == _currentChild) {
                return readFirstChild(out);
            }
            else if (_currentChild < _children.size() - 1) {
                return hashOtherChildren(out);
            }
            else {
                _hashingChildren = false;
                // We don't hash our last child.  Instead, we probe the table created from the
                // previous children, returning results in the order of the last child.
                // Fall through to below.
            }
        }

        // Returning results.  We read from the last child and return the results that are in our
        // hash map.

        // We should be EOF if we're not hashing results and the dataMap is empty.
        verify(!_dataMap.empty());

        // We probe _dataMap with the last child.
        verify(_currentChild == _children.size() - 1);

        // Get the next result for the (_children.size() - 1)-th child.
        StageState childStatus = workChild(_children.size() - 1, out);
        if (PlanStage::ADVANCED != childStatus) {
            return childStatus;
        }

        // We know that we've ADVANCED.  See if the WSM is in our table.
        WorkingSetMember* member = _ws->get(*out);

        // Maybe the child had an invalidation.  We intersect DiskLoc(s) so we can't do anything
        // with this WSM.
        if (!member->hasLoc()) {
            _ws->flagForReview(*out);
            return PlanStage::NEED_TIME;
        }

        DataMap::iterator it = _dataMap.find(member->loc);
        if (_dataMap.end() == it) {
            // Child's output wasn't in every previous child.  Throw it out.
            _ws->free(*out);
            ++_commonStats.needTime;
            return PlanStage::NEED_TIME;
        }
        else {
            // Child's output was in every previous child.  Merge any key data in
            // the child's output and free the child's just-outputted WSM.
            WorkingSetID hashID = it->second;
            _dataMap.erase(it);

            WorkingSetMember* olderMember = _ws->get(hashID);
            AndCommon::mergeFrom(olderMember, *member);
            _ws->free(*out);

            // We should check for matching at the end so the matcher can use information in the
            // indices of all our children.
            if (Filter::passes(olderMember, _filter)) {
                *out = hashID;
                ++_commonStats.advanced;
                return PlanStage::ADVANCED;
            }
            else {
                _ws->free(hashID);
                ++_commonStats.needTime;
                return PlanStage::NEED_TIME;
            }
        }
    }

    PlanStage::StageState AndHashStage::workChild(size_t childNo, WorkingSetID* out) {
        if (WorkingSet::INVALID_ID != _lookAheadResults[childNo]) {
            *out = _lookAheadResults[childNo];
            _lookAheadResults[childNo] = WorkingSet::INVALID_ID;
            return PlanStage::ADVANCED;
        }
        else {
            return _children[childNo]->work(out);
        }
    }

    PlanStage::StageState AndHashStage::readFirstChild(WorkingSetID* out) {
        verify(_currentChild == 0);

        WorkingSetID id = WorkingSet::INVALID_ID;
        StageState childStatus = workChild(0, &id);

        if (PlanStage::ADVANCED == childStatus) {
            WorkingSetMember* member = _ws->get(id);

            // Maybe the child had an invalidation.  We intersect DiskLoc(s) so we can't do anything
            // with this WSM.
            if (!member->hasLoc()) {
                _ws->flagForReview(id);
                return PlanStage::NEED_TIME;
            }

            verify(member->hasLoc());
            verify(_dataMap.end() == _dataMap.find(member->loc));

            _dataMap[member->loc] = id;

            // Update memory stats.
            _memUsage += member->getMemUsage();

            ++_commonStats.needTime;
            return PlanStage::NEED_TIME;
        }
        else if (PlanStage::IS_EOF == childStatus) {
            // Done reading child 0.
            _currentChild = 1;

            // If our first child was empty, don't scan any others, no possible results.
            if (_dataMap.empty()) {
                _hashingChildren = false;
                return PlanStage::IS_EOF;
            }

            ++_commonStats.needTime;
            _specificStats.mapAfterChild.push_back(_dataMap.size());

            return PlanStage::NEED_TIME;
        }
        else if (PlanStage::FAILURE == childStatus) {
            *out = id;
            // If a stage fails, it may create a status WSM to indicate why it
            // failed, in which case 'id' is valid.  If ID is invalid, we
            // create our own error message.
            if (WorkingSet::INVALID_ID == id) {
                mongoutils::str::stream ss;
                ss << "hashed AND stage failed to read in results to from first child";
                Status status(ErrorCodes::InternalError, ss);
                *out = WorkingSetCommon::allocateStatusMember( _ws, status);
            }
            return childStatus;
        }
        else {
            if (PlanStage::NEED_TIME == childStatus) {
                ++_commonStats.needTime;
            }

            return childStatus;
        }
    }

    PlanStage::StageState AndHashStage::hashOtherChildren(WorkingSetID* out) {
        verify(_currentChild > 0);

        WorkingSetID id = WorkingSet::INVALID_ID;
        StageState childStatus = workChild(_currentChild, &id);

        if (PlanStage::ADVANCED == childStatus) {
            WorkingSetMember* member = _ws->get(id);

            // Maybe the child had an invalidation.  We intersect DiskLoc(s) so we can't do anything
            // with this WSM.
            if (!member->hasLoc()) {
                _ws->flagForReview(id);
                return PlanStage::NEED_TIME;
            }

            verify(member->hasLoc());
            if (_dataMap.end() == _dataMap.find(member->loc)) {
                // Ignore.  It's not in any previous child.
            }
            else {
                // We have a hit.  Copy data into the WSM we already have.
                _seenMap.insert(member->loc);
                WorkingSetMember* olderMember = _ws->get(_dataMap[member->loc]);
                size_t memUsageBefore = olderMember->getMemUsage();

                AndCommon::mergeFrom(olderMember, *member);

                // Update memory stats.
                _memUsage += olderMember->getMemUsage() - memUsageBefore;
            }
            _ws->free(id);
            ++_commonStats.needTime;
            return PlanStage::NEED_TIME;
        }
        else if (PlanStage::IS_EOF == childStatus) {
            // Finished with a child.
            ++_currentChild;

            // Keep elements of _dataMap that are in _seenMap.
            DataMap::iterator it = _dataMap.begin();
            while (it != _dataMap.end()) {
                if (_seenMap.end() == _seenMap.find(it->first)) {
                    DataMap::iterator toErase = it;
                    ++it;

                    // Update memory stats.
                    WorkingSetMember* member = _ws->get(toErase->second);
                    _memUsage -= member->getMemUsage();

                    _ws->free(toErase->second);
                    _dataMap.erase(toErase);
                }
                else { ++it; }
            }

            _specificStats.mapAfterChild.push_back(_dataMap.size());

            _seenMap.clear();

            // _dataMap is now the intersection of the first _currentChild nodes.

            // If we have nothing to AND with after finishing any child, stop.
            if (_dataMap.empty()) {
                _hashingChildren = false;
                return PlanStage::IS_EOF;
            }

            // We've finished scanning all children.  Return results with the next call to work().
            if (_currentChild == _children.size()) {
                _hashingChildren = false;
            }

            ++_commonStats.needTime;
            return PlanStage::NEED_TIME;
        }
        else if (PlanStage::FAILURE == childStatus) {
            *out = id;
            // If a stage fails, it may create a status WSM to indicate why it
            // failed, in which case 'id' is valid.  If ID is invalid, we
            // create our own error message.
            if (WorkingSet::INVALID_ID == id) {
                mongoutils::str::stream ss;
                ss << "hashed AND stage failed to read in results from other child "
                   << _currentChild;
                Status status(ErrorCodes::InternalError, ss);
                *out = WorkingSetCommon::allocateStatusMember( _ws, status);
            }
            return childStatus;
        }
        else {
            if (PlanStage::NEED_TIME == childStatus) {
                ++_commonStats.needTime;
            }

            return childStatus;
        }
    }

    void AndHashStage::saveState() {
        ++_commonStats.yields;

        for (size_t i = 0; i < _children.size(); ++i) {
            _children[i]->saveState();
        }
    }

    void AndHashStage::restoreState(OperationContext* opCtx) {
        _txn = opCtx;
        ++_commonStats.unyields;

        for (size_t i = 0; i < _children.size(); ++i) {
            _children[i]->restoreState(opCtx);
        }
    }

    void AndHashStage::invalidate(const DiskLoc& dl, InvalidationType type) {
        ++_commonStats.invalidates;

        if (isEOF()) { return; }

        for (size_t i = 0; i < _children.size(); ++i) {
            _children[i]->invalidate(dl, type);
        }

        // Invalidation can happen to our warmup results.  If that occurs just
        // flag it and forget about it.
        for (size_t i = 0; i < _lookAheadResults.size(); ++i) {
            if (WorkingSet::INVALID_ID != _lookAheadResults[i]) {
                WorkingSetMember* member = _ws->get(_lookAheadResults[i]);
                if (member->hasLoc() && member->loc == dl) {
                    WorkingSetCommon::fetchAndInvalidateLoc(_txn, member, _collection);
                    _ws->flagForReview(_lookAheadResults[i]);
                    _lookAheadResults[i] = WorkingSet::INVALID_ID;
                }
            }
        }

        // If it's a deletion, we have to forget about the DiskLoc, and since the AND-ing is by
        // DiskLoc we can't continue processing it even with the object.
        //
        // If it's a mutation the predicates implied by the AND-ing may no longer be true.
        //
        // So, we flag and try to pick it up later.
        DataMap::iterator it = _dataMap.find(dl);
        if (_dataMap.end() != it) {
            WorkingSetID id = it->second;
            WorkingSetMember* member = _ws->get(id);
            verify(member->loc == dl);

            if (_hashingChildren) {
                ++_specificStats.flaggedInProgress;
            }
            else {
                ++_specificStats.flaggedButPassed;
            }

            // Update memory stats.
            _memUsage -= member->getMemUsage();

            // The loc is about to be invalidated.  Fetch it and clear the loc.
            WorkingSetCommon::fetchAndInvalidateLoc(_txn, member, _collection);

            // Add the WSID to the to-be-reviewed list in the WS.
            _ws->flagForReview(id);

            // And don't return it from this stage.
            _dataMap.erase(it);
        }
    }

    vector<PlanStage*> AndHashStage::getChildren() const {
        return _children;
    }

    PlanStageStats* AndHashStage::getStats() {
        _commonStats.isEOF = isEOF();

        _specificStats.memLimit = _maxMemUsage;
        _specificStats.memUsage = _memUsage;

        // Add a BSON representation of the filter to the stats tree, if there is one.
        if (NULL != _filter) {
            BSONObjBuilder bob;
            _filter->toBSON(&bob);
            _commonStats.filter = bob.obj();
        }

        auto_ptr<PlanStageStats> ret(new PlanStageStats(_commonStats, STAGE_AND_HASH));
        ret->specific.reset(new AndHashStats(_specificStats));
        for (size_t i = 0; i < _children.size(); ++i) {
            ret->children.push_back(_children[i]->getStats());
        }

        return ret.release();
    }

    const CommonStats* AndHashStage::getCommonStats() {
        return &_commonStats;
    }

    const SpecificStats* AndHashStage::getSpecificStats() {
        return &_specificStats;
    }

}  // namespace mongo
