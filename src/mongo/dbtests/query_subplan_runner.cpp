/**
 *    Copyright (C) 2014 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/client/dbclientcursor.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/get_runner.h"
#include "mongo/db/query/subplan_runner.h"
#include "mongo/dbtests/dbtests.h"

namespace QueryStageSubplan {

    class QuerySubplanRunnerBase {
    public:
        QuerySubplanRunnerBase() { }

        virtual ~QuerySubplanRunnerBase() {
            Client::WriteContext ctx(ns());
            _client.dropCollection(ns());
        }

        void addIndex(const BSONObj& obj) {
            _client.ensureIndex(ns(), obj);
        }

        void insert(const BSONObj& doc) {
            _client.insert(ns(), doc);
        }

        static const char* ns() { return "unittests.QuerySubplanRunner"; }

    private:
        static DBDirectClient _client;
    };

    DBDirectClient QuerySubplanRunnerBase::_client;

    /**
     * SERVER-15012: test that the subplan stage does not crash when the winning solution
     * for an $or clause uses a '2d' index. We don't produce cache data for '2d'. The subplanner
     * should gracefully fail after finding that no cache data is available, allowing us to fall
     * back to regular planning.
     */
    class QuerySubplanRunnerGeo2dOr : public QuerySubplanRunnerBase {
    public:
        void run() {
            Client::WriteContext ctx(ns());
            addIndex(BSON("a" << "2d"));
            addIndex(BSON("b" << 1));

            BSONObj query =
                fromjson("{$or: [{a: {$geoWithin: {$box: [[0,0],[1,1]]}}, b: 1},"
                                "{a: {$geoWithin: {$box: [[1,1],[1,1]]}}, b: 1}]}");

            CanonicalQuery* cq;
            ASSERT_OK(CanonicalQuery::canonicalize(ns(), query, &cq));

            Collection* collection = ctx.ctx().db()->getCollection(ns());

            // Get planner params.
            QueryPlannerParams plannerParams;
            fillOutPlannerParams(collection, cq, &plannerParams);

            // Create the subplan runner.
            SubplanRunner* subplanRunner;
            ASSERT_OK(SubplanRunner::make(collection, plannerParams, cq, &subplanRunner));

            // On success, the subplan runner should be non-NULL.
            ASSERT(subplanRunner);

            // Make sure that we can run the plan to EOF without crashing.
            BSONObj objOut;
            Runner::RunnerState state = Runner::RUNNER_ADVANCED;
            while (Runner::RUNNER_ADVANCED == state) {
                state = subplanRunner->getNext(&objOut, NULL);
            }

            // Make sure we hit EOF successfully.
            ASSERT(Runner::RUNNER_EOF == state);
        }
    };

    class All : public Suite {
    public:
        All() : Suite("query_subplan_runner") {}

        void setupTests() {
            add<QuerySubplanRunnerGeo2dOr>();
        }
    } all;

} // namespace QueryStageSubplan
