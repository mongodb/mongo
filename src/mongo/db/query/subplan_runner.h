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
#include <queue>

#include "mongo/base/status.h"
#include "mongo/db/query/runner.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/query_solution.h"

namespace mongo {

    class BSONObj;
    class CanonicalQuery;
    class DiskLoc;
    class TypeExplain;
    struct PlanInfo;

    class SubplanRunner : public Runner {
    public:
        /**
         * Used to create SubplanRunner instances. The caller owns the instance
         * returned through 'out'.
         *
         * 'out' is valid only if an OK status is returned.
         */
        static Status make(Collection* collection,
                           const QueryPlannerParams& params,
                           CanonicalQuery* cq,
                           SubplanRunner** out);

        static bool canUseSubplanRunner(const CanonicalQuery& query);

        virtual ~SubplanRunner();

        virtual Runner::RunnerState getNext(BSONObj* objOut, DiskLoc* dlOut);

        virtual bool isEOF();

        virtual void saveState();

        virtual bool restoreState();

        virtual void setYieldPolicy(Runner::YieldPolicy policy);

        virtual void invalidate(const DiskLoc& dl, InvalidationType type);

        virtual const std::string& ns();

        virtual void kill();

        virtual const Collection* collection() {
            return _collection;
        }

        virtual Status getInfo(TypeExplain** explain,
                               PlanInfo** planInfo) const;

        /**
         * Plan each branch of the $or independently, and store the resulting
         * lists of query solutions in '_solutions'.
         *
         * Called from SubplanRunner::make so that getRunner can fail if
         * subquery planning fails, rather than returning a runner and failing
         * through getNext(...).
         */
        Status planSubqueries();

    private:
        SubplanRunner(Collection* collection,
                      const QueryPlannerParams& params,
                      CanonicalQuery* cq);

        bool runSubplans();

        enum SubplanRunnerState {
            PLANNING,
            RUNNING,
        };

        SubplanRunnerState _state;

        Collection* _collection;

        QueryPlannerParams _plannerParams;

        std::auto_ptr<CanonicalQuery> _query;

        bool _killed;

        Runner::YieldPolicy _policy;

        boost::scoped_ptr<Runner> _underlyingRunner;

        std::string _ns;

        // We do the subquery planning up front, and keep the resulting
        // query solutions here. Lists of query solutions are dequeued
        // and ownership is transferred to the underlying runners one
        // at a time.
        std::queue< std::vector<QuerySolution*> > _solutions;

        // Holds the canonicalized subqueries. Ownership is transferred
        // to the underlying runners one at a time.
        std::queue<CanonicalQuery*> _cqs;

        // We need this to extract cache-friendly index data from the index assignments.
        map<BSONObj, size_t> _indexMap;
    };

}  // namespace mongo
