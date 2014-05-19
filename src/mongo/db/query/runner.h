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

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/invalidation_type.h"

namespace mongo {

    class Collection;
    class DiskLoc;
    class TypeExplain;
    struct PlanInfo;

    /**
     * A runner runs a query.
     */
    class Runner {
    public:
        virtual ~Runner() { }

        enum RunnerState {
            // We successfully populated the out parameter.
            RUNNER_ADVANCED,

            // We're EOF.  We won't return any more results (edge case exception: capped+tailable).
            RUNNER_EOF,

            // We were killed or had an error.
            RUNNER_DEAD,

            // getNext was asked for data it cannot provide, or the underlying PlanStage had an
            // unrecoverable error.
            // If the underlying PlanStage has any information on the error, it will be available in
            // the objOut parameter. Call WorkingSetCommon::toStatusString() to retrieve the error
            // details from the output BSON object.
            RUNNER_ERROR,
        };

        static std::string statestr(RunnerState s) {
            if (RUNNER_ADVANCED == s) {
                return "RUNNER_ADVANCED";
            }
            else if (RUNNER_EOF == s) {
                return "RUNNER_EOF";
            }
            else if (RUNNER_DEAD == s) {
                return "RUNNER_DEAD";
            }
            else {
                verify(RUNNER_ERROR == s);
                return "RUNNER_ERROR";
            }
        }

        /**
         * Get the next result from the query.
         *
         * If objOut is not NULL, only results that have a BSONObj are returned.  The BSONObj may
         * point to on-disk data (isOwned will be false) and must be copied by the caller before
         * yielding.
         *
         * If dlOut is not NULL, only results that have a valid DiskLoc are returned.
         *
         * If both objOut and dlOut are not NULL, only results with both a valid BSONObj and DiskLoc
         * will be returned.  The BSONObj is the object located at the DiskLoc provided.
         *
         * If the underlying query machinery produces a result that does not have the data requested
         * by the user, it will be silently dropped.
         *
         * If the caller is running a query, they probably only care about the object.
         * If the caller is an internal client, they may only care about DiskLocs (index scan), or
         * about object + DiskLocs (collection scan).
         *
         * Some notes on objOut and ownership:
         *
         * objOut may be an owned object in certain cases: invalidation of the underlying DiskLoc,
         * the object is created from covered index key data, the object is projected or otherwise
         * the result of a computation.
         *
         * objOut will also be owned when the underlying PlanStage has provided error details in the
         * event of a RUNNER_ERROR. Call WorkingSetCommon::toStatusString() to convert the object
         * to a loggable format.
         *
         * objOut will be unowned if it's the result of a fetch or a collection scan.
         */
        virtual RunnerState getNext(BSONObj* objOut, DiskLoc* dlOut) = 0;

        /**
         * Will the next call to getNext() return EOF?  It's useful to know if the runner is done
         * without having to take responsibility for a result.
         */
        virtual bool isEOF() = 0;

        /**
         * Inform the runner about changes to DiskLoc(s) that occur while the runner is yielded.
         * The runner must take any actions required to continue operating correctly, including
         * broadcasting the invalidation request to the PlanStage tree being run.
         *
         * Called from CollectionCursorCache::invalidateDocument.
         *
         * See db/invalidation_type.h for InvalidationType.
         */
        virtual void invalidate(const DiskLoc& dl, InvalidationType type) = 0;

        /**
         * Mark the Runner as no longer valid.  Can happen when a runner yields and the underlying
         * database is dropped/indexes removed/etc.  All future to calls to getNext return
         * RUNNER_DEAD. Every other call is a NOOP.
         *
         * The runner must guarantee as a postcondition that future calls to collection() will
         * return NULL.
         */
        virtual void kill() = 0;

        /**
         * Save any state required to yield.
         */
        virtual void saveState() = 0;

        /**
         * Restore saved state, possibly after a yield.  Return true if the runner is OK, false if
         * it was killed.
         */
        virtual bool restoreState() = 0;

        /**
         * Return the NS that the query is running over.
         */
        virtual const std::string& ns() = 0;

        /**
         * Return the Collection that the query is running over.
         */
        virtual const Collection* collection() = 0;

        /**
         * Returns OK, allocating and filling '*explain' or '*planInfo' with a description of the
         * chosen plan, depending on which is non-NULL (one of the two should be NULL). Caller
         * takes onwership of either '*explain' and '*planInfo'. Otherwise, returns false
         * a detailed error status.
         *
         * If 'explain' is NULL, then this out-parameter is ignored. Similarly, if 'staticInfo'
         * is NULL, then no static debug information is produced.
         */
        virtual Status getInfo(TypeExplain** explain, PlanInfo** planInfo) const = 0;
    };

}  // namespace mongo
