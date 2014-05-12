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

#include <boost/scoped_ptr.hpp>
#include <string>

#include "mongo/base/status.h"
#include "mongo/db/query/runner.h"

namespace mongo {

    class BSONObj;
    class CanonicalQuery;
    class DiskLoc;
    class PlanExecutor;
    class PlanStage;
    struct QuerySolution;
    class TypeExplain;
    struct PlanInfo;
    class WorkingSet;

    /**
     * This is a runner that was requested by an internal client of the query system, as opposed to
     * runners that are built in response to a query entering the system. It is only used by
     * internal clients of the query systems (e.g., chunk migration, index building, commands that
     * traverse data such as md5, ... )
     *
     * The salient feature of this Runner is that it does not interact with the cache at all.
     */
    class InternalRunner : public Runner {
    public:

        /** Takes ownership of root and ws. */
        InternalRunner(const Collection* collection, PlanStage* root, WorkingSet* ws);

        virtual ~InternalRunner();

        Runner::RunnerState getNext(BSONObj* objOut, DiskLoc* dlOut);

        virtual bool isEOF();

        virtual void saveState();

        virtual bool restoreState();

        virtual const std::string& ns();

        virtual void invalidate(const DiskLoc& dl, InvalidationType type);

        virtual void kill();

        virtual const Collection* collection() { return _collection; }

        /**
         * Returns OK, allocating and filling in '*explain' with details of the plan used by
         * this runner. Caller takes ownership of '*explain'. Similarly fills in '*planInfo',
         * which the caller takes ownership of. Otherwise, return a status describing the
         * error.
         *
         * Strictly speaking, an InternalRunner's explain is never exposed, simply because an
         * InternalRunner itself is not exposed. But we implement the explain here anyway so
         * to help in debugging situations.
         */
        virtual Status getInfo(TypeExplain** explain,
                               PlanInfo** planInfo) const;

    private:
        const Collection* _collection;

        boost::scoped_ptr<PlanExecutor> _exec;
    };

}  // namespace mongo
