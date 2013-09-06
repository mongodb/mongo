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

namespace mongo {

    bool DeletedRangeCmp::operator()(const DeletedRange& lhs,
                                     const DeletedRange& rhs) const {
        const int nsComp = lhs.ns.compare(rhs.ns);

       if (nsComp < 0) {
           return true;
       }

       if (nsComp > 0) {
           return false;
       }

       return compareRanges(lhs.min, lhs.max, rhs.min, rhs.max) < 0;
    }

    RangeDeleterMockEnv::RangeDeleterMockEnv():
        _deleteListMutex("delList"),
        _cursorMapMutex("cursorMap"),
        _pauseDeleteMutex("pauseDelete"),
        _pauseDelete(false),
        _pausedCount(0),
        _envStatMutex("envStat"),
        _getCursorsCallCount(0) {
    }

    void RangeDeleterMockEnv::addCursorId(const StringData& ns, CursorId id) {
        scoped_lock sl(_cursorMapMutex);
        _cursorMap[ns.toString()].insert(id);
    }

    void RangeDeleterMockEnv::removeCursorId(const StringData& ns, CursorId id) {
        scoped_lock sl(_cursorMapMutex);
        _cursorMap[ns.toString()].erase(id);
    }

    void RangeDeleterMockEnv::pauseDeletes() {
        scoped_lock sl(_pauseDeleteMutex);
        _pauseDelete = true;
    }

    void RangeDeleterMockEnv::resumeOneDelete() {
        scoped_lock sl(_pauseDeleteMutex);
        _pauseDelete = false;
        _pausedCV.notify_one();
    }

    void RangeDeleterMockEnv::waitForNthGetCursor(uint64_t nthCall) {
        scoped_lock sl(_envStatMutex);
        while (_getCursorsCallCount < nthCall) {
            _cursorsCallCountUpdatedCV.wait(sl.boost());
        }
    }

    void RangeDeleterMockEnv::waitForNthPausedDelete(uint64_t nthPause) {
        scoped_lock sl(_pauseDeleteMutex);
        while(_pausedCount < nthPause) {
            _pausedDeleteChangeCV.wait(sl.boost());
        }
    }

    bool RangeDeleterMockEnv::deleteOccured() const {
        scoped_lock sl(_deleteListMutex);
        return !_deleteList.empty();
    }

    DeletedRange RangeDeleterMockEnv::getLastDelete() const {
        scoped_lock sl(_deleteListMutex);
        return _deleteList.back();
    }

    bool RangeDeleterMockEnv::deleteRange(const StringData& ns,
                                          const BSONObj& min,
                                          const BSONObj& max,
                                          const BSONObj& shardKeyPattern,
                                          bool secondaryThrottle,
                                          string* errMsg) {

        {
            scoped_lock sl(_pauseDeleteMutex);
            bool wasInitiallyPaused = _pauseDelete;

            if (_pauseDelete) {
                _pausedCount++;
                _pausedDeleteChangeCV.notify_one();
            }

            while (_pauseDelete) {
                _pausedCV.wait(sl.boost());
            }

            _pauseDelete = wasInitiallyPaused;
        }

        {
            scoped_lock sl(_deleteListMutex);

            DeletedRange entry;
            entry.ns = ns.toString();
            entry.min = min.getOwned();
            entry.max = max.getOwned();
            entry.shardKeyPattern = shardKeyPattern.getOwned();

            _deleteList.push_back(entry);
        }

        return true;
    }

    void RangeDeleterMockEnv::getCursorIds(const StringData& ns, set<CursorId>* in) {
        {
            scoped_lock sl(_cursorMapMutex);
            const set<CursorId>& _cursors = _cursorMap[ns.toString()];
            std::copy(_cursors.begin(), _cursors.end(), inserter(*in, in->begin()));
        }

        {
            scoped_lock sl(_envStatMutex);
            _getCursorsCallCount++;
            _cursorsCallCountUpdatedCV.notify_one();
        }
    }
}
