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

#include "mongo/db/range_deleter_mock_env.h"

#include "mongo/db/service_context.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/stdx/memory.h"

namespace mongo {

using std::set;
using std::string;

bool DeletedRangeCmp::operator()(const DeletedRange& lhs, const DeletedRange& rhs) const {
    const int nsComp = lhs.ns.compare(rhs.ns);

    if (nsComp < 0) {
        return true;
    }

    if (nsComp > 0) {
        return false;
    }

    return compareRanges(lhs.min, lhs.max, rhs.min, rhs.max) < 0;
}

RangeDeleterMockEnv::RangeDeleterMockEnv()
    : _pauseDelete(false), _pausedCount(0), _getCursorsCallCount(0) {
    setGlobalServiceContext(stdx::make_unique<ServiceContextNoop>());
}

void RangeDeleterMockEnv::addCursorId(StringData ns, CursorId id) {
    stdx::lock_guard<stdx::mutex> sl(_cursorMapMutex);
    _cursorMap[ns.toString()].insert(id);
}

void RangeDeleterMockEnv::removeCursorId(StringData ns, CursorId id) {
    stdx::lock_guard<stdx::mutex> sl(_cursorMapMutex);
    _cursorMap[ns.toString()].erase(id);
}

void RangeDeleterMockEnv::pauseDeletes() {
    stdx::lock_guard<stdx::mutex> sl(_pauseDeleteMutex);
    _pauseDelete = true;
}

void RangeDeleterMockEnv::resumeOneDelete() {
    stdx::lock_guard<stdx::mutex> sl(_pauseDeleteMutex);
    _pauseDelete = false;
    _pausedCV.notify_one();
}

void RangeDeleterMockEnv::waitForNthGetCursor(uint64_t nthCall) {
    stdx::unique_lock<stdx::mutex> sl(_envStatMutex);
    while (_getCursorsCallCount < nthCall) {
        _cursorsCallCountUpdatedCV.wait(sl);
    }
}

void RangeDeleterMockEnv::waitForNthPausedDelete(uint64_t nthPause) {
    stdx::unique_lock<stdx::mutex> sl(_pauseDeleteMutex);
    while (_pausedCount < nthPause) {
        _pausedDeleteChangeCV.wait(sl);
    }
}

bool RangeDeleterMockEnv::deleteOccured() const {
    stdx::lock_guard<stdx::mutex> sl(_deleteListMutex);
    return !_deleteList.empty();
}

DeletedRange RangeDeleterMockEnv::getLastDelete() const {
    stdx::lock_guard<stdx::mutex> sl(_deleteListMutex);
    return _deleteList.back();
}

bool RangeDeleterMockEnv::deleteRange(OperationContext* txn,
                                      const RangeDeleteEntry& taskDetails,
                                      long long int* deletedDocs,
                                      string* errMsg) {
    {
        stdx::unique_lock<stdx::mutex> sl(_pauseDeleteMutex);
        bool wasInitiallyPaused = _pauseDelete;

        if (_pauseDelete) {
            _pausedCount++;
            _pausedDeleteChangeCV.notify_one();
        }

        while (_pauseDelete) {
            _pausedCV.wait(sl);
        }

        _pauseDelete = wasInitiallyPaused;
    }

    {
        stdx::lock_guard<stdx::mutex> sl(_deleteListMutex);

        DeletedRange entry;
        entry.ns = taskDetails.options.range.ns;
        entry.min = taskDetails.options.range.minKey.getOwned();
        entry.max = taskDetails.options.range.maxKey.getOwned();
        entry.shardKeyPattern = taskDetails.options.range.keyPattern.getOwned();

        _deleteList.push_back(entry);
    }

    return true;
}

void RangeDeleterMockEnv::getCursorIds(OperationContext* txn, StringData ns, set<CursorId>* in) {
    {
        stdx::lock_guard<stdx::mutex> sl(_cursorMapMutex);
        const set<CursorId>& _cursors = _cursorMap[ns.toString()];
        std::copy(_cursors.begin(), _cursors.end(), inserter(*in, in->begin()));
    }

    {
        stdx::lock_guard<stdx::mutex> sl(_envStatMutex);
        _getCursorsCallCount++;
        _cursorsCallCountUpdatedCV.notify_one();
    }
}
}
