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
#include "mongo/db/query/plan_cache.h"
#include "mongo/db/query/runner.h"

namespace mongo {

    class BSONObj;
    class CachedSolution;
    class CanonicalQuery;
    class DiskLoc;
    class PlanExecutor;
    class PlanStage;
    class TypeExplain;
    class WorkingSet;

    /**
     * CachedPlanRunner runs a plan retrieved from the cache.
     *
     * If we run a plan from the cache and behavior wildly deviates from expected behavior, we may
     * remove the plan from the cache.  See plan_cache.h.
     */
    class CachedPlanRunner : public Runner {
    public:
        /**
         * Takes ownership of all arguments.
         * XXX: what args should this really take?  probably a cachekey as well?
         */
        CachedPlanRunner(CanonicalQuery* canonicalQuery, QuerySolution* solution,
                         PlanStage* root, WorkingSet* ws);

        virtual ~CachedPlanRunner();

        Runner::RunnerState getNext(BSONObj* objOut, DiskLoc* dlOut);

        virtual bool isEOF();

        virtual void saveState();

        virtual bool restoreState();

        virtual void invalidate(const DiskLoc& dl, InvalidationType type);

        virtual void setYieldPolicy(Runner::YieldPolicy policy);

        virtual const std::string& ns();

        virtual void kill();

        /**
         * Returns OK, allocating and filling in '*explain' with details of the cached
         * plan. Caller takes ownership of '*explain'. Otherwise, return a status describing
         * the error.
         */
        virtual Status getExplainPlan(TypeExplain** explain) const;

        /**
         * Takes ownership of all arguments.
         */
        void setBackupPlan(QuerySolution* qs, PlanStage* root, WorkingSet* ws);

    private:
        void updateCache();

        boost::scoped_ptr<CanonicalQuery> _canonicalQuery;
        boost::scoped_ptr<QuerySolution> _solution;
        boost::scoped_ptr<PlanExecutor> _exec;

        // Owned here. If non-NULL, then this plan executor is capable
        // of executing a backup plan in the case of a blocking sort.
        std::auto_ptr<PlanExecutor> _backupPlan;

        // Owned here. If non-NULL, contains the query solution corresponding
        // to the backup plan.
        boost::scoped_ptr<QuerySolution> _backupSolution;

        // Whether the executor for the winning plan has produced results yet.
        bool _alreadyProduced;

        // Have we updated the cache with our plan stats yet?
        bool _updatedCache;

        // Has the runner been killed?
        bool _killed;
    };

}  // namespace mongo
