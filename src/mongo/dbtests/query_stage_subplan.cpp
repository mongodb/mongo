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

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/exec/subplan.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/dbtests/dbtests.h"

namespace QueryStageSubplan {

    class QueryStageSubplanBase {
    public:
        QueryStageSubplanBase()
            : _client(&_txn) { }

        virtual ~QueryStageSubplanBase() {
            Client::WriteContext ctx(&_txn, ns());
            _client.dropCollection(ns());
            ctx.commit();
        }

        void addIndex(const BSONObj& obj) {
            _client.ensureIndex(ns(), obj);
        }

        void insert(const BSONObj& doc) {
            _client.insert(ns(), doc);
        }

        static const char* ns() { return "unittests.QueryStageSubplan"; }

    protected:
        OperationContextImpl _txn;

    private:
        DBDirectClient _client;
    };

    /**
     * SERVER-15012: test that the subplan stage does not crash when the winning solution
     * for an $or clause uses a '2d' index. We don't produce cache data for '2d'. The subplanner
     * should gracefully fail after finding that no cache data is available, allowing us to fall
     * back to regular planning.
     */
    class QueryStageSubplanGeo2dOr : public QueryStageSubplanBase {
    public:
        void run() {
            Client::WriteContext ctx(&_txn, ns());
            addIndex(BSON("a" << "2d" << "b" << 1));
            addIndex(BSON("a" << "2d"));

            BSONObj query = fromjson("{$or: [{a: {$geoWithin: {$centerSphere: [[0,0],10]}}},"
                                            "{a: {$geoWithin: {$centerSphere: [[1,1],10]}}}]}");

            CanonicalQuery* cq;
            ASSERT_OK(CanonicalQuery::canonicalize(ns(), query, &cq));

            Collection* collection = ctx.ctx().db()->getCollection(&_txn, ns());

            // Get planner params.
            QueryPlannerParams plannerParams;
            fillOutPlannerParams(&_txn, collection, cq, &plannerParams);

            // We expect creation of the subplan stage to fail.
            WorkingSet ws;
            SubplanStage* subplan;
            ASSERT_NOT_OK(SubplanStage::make(&_txn, collection, &ws, plannerParams, cq, &subplan));

            ctx.commit();
        }
    };

    class All : public Suite {
    public:
        All() : Suite("query_stage_subplan") {}

        void setupTests() {
            add<QueryStageSubplanGeo2dOr>();
        }
    } all;

} // namespace QueryStageSubplan
