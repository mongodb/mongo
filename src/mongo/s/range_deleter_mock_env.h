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

#pragma once

#include <map>
#include <set>
#include <string>

#include "mongo/s/range_arithmetic.h"
#include "mongo/s/range_deleter.h"

namespace mongo {

    struct DeletedRange {
        std::string ns;
        BSONObj min;
        BSONObj max;
    };

    /**
     * Comparator function object compatible with std::set.
     */
    struct DeletedRangeCmp {
        bool operator()(const DeletedRange& lhs, const DeletedRange& rhs) const;
    };

    /**
     * Mock environment for RangeDeleter with knobs for pausing/resuming
     * deletes, setting open cursors IDs per namespace and the ability to
     * record the history of deletes performed through this environment.
     */
    class RangeDeleterMockEnv: public mongo::RangeDeleterEnv {
    public:
        RangeDeleterMockEnv();

        //
        // Environment modification methods.
        //

        /**
         * Adds an id to the current set of cursors in the given namespace.
         */
        void addCursorId(const StringData& ns, CursorId id);

        /**
         * Removes the id from the set of open cursors in the given namespace.
         */
        void removeCursorId(const StringData& ns, CursorId id);

        //
        // Environment synchronization methods.
        //

        /**
         * Blocks all new deletes from proceeding.
         */
        void pauseDeletes();

        /**
         * Unblocks one paused delete.
         */
        void resumeOneDelete();

        /**
         * Blocks until the getCursor method was called and terminated at least the
         * specified number of times for the entire lifetime of this deleter.
         */
        void waitForNthGetCursor(uint64_t nthCall);

        /**
         * Blocks until the deleteRange method was called and at the same time paused
         * at least the specified number of times for the entire lifetime of this deleter.
         */
        void waitForNthPausedDelete(uint64_t nthPause);

        //
        // Environment introspection methods.
        //

        /**
         * Returns true if deleteRange was called at least once.
         */
        bool deleteOccured() const;

        /**
         * Returns the last delete. Undefined if deleteOccured is false.
         */
        DeletedRange getLastDelete() const;

        //
        // Environment methods.
        //

        /**
         * Basic implementation of delete that matches the signature for
         * RangeDeleterEnv::deleteRange. This does not actually perform the delete
         * but simply keeps a record of it. Can also be paused by pauseDeletes and
         * resumed with resumeDeletes.
         */
        bool deleteRange(const StringData& ns,
                         const BSONObj& min,
                         const BSONObj& max,
                         bool secondaryThrottle,
                         string* errMsg);

        /**
         * Basic implementation of gathering open cursors that matches the signature for
         * RangeDeleterEnv::getCursorIds. The cursors returned can be modified with
         * the setCursorId and clearCursorMap methods.
         */
        void getCursorIds(const StringData& ns, set<CursorId>* in);

    private:
        // mutex acquisition ordering:
        // _envStatMutex -> _pauseDeleteMutex -> _deleteListMutex -> _cursorMapMutex

        mutable mutex _deleteListMutex;
        std::vector<DeletedRange> _deleteList;

        mutex _cursorMapMutex;
        std::map<std::string, std::set<CursorId> > _cursorMap;

        // Protects _pauseDelete & _pausedCount
        mutex _pauseDeleteMutex;
        boost::condition _pausedCV;
        bool _pauseDelete;

        // Number of times a delete gets paused.
        uint64_t _pausedCount;
        // _pausedCount < nthPause (used by waitForNthPausedDelete)
        boost::condition _pausedDeleteChangeCV;

        // Protects all variables below this line.
        mutex _envStatMutex;

        // Keeps track of the number of times getCursorIds was called.
        uint64_t _getCursorsCallCount;
        // _getCursorsCallCount < nthCall (used by waitForNthGetCursor)
        boost::condition _cursorsCallCountUpdatedCV;
    };
}
