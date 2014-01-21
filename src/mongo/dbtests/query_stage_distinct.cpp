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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/client/dbclientcursor.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/exec/distinct_scan.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/instance.h"
#include "mongo/db/json.h"
#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/dbtests/dbtests.h"

/**
 * This file tests db/exec/distinct.cpp
 */

namespace QueryStageDistinct {

    class DistinctBase {
    public:
        DistinctBase() { }

        virtual ~DistinctBase() {
            Client::WriteContext ctx(ns());
            _client.dropCollection(ns());
        }

        void addIndex(const BSONObj& obj) {
            Client::WriteContext ctx(ns());
            _client.ensureIndex(ns(), obj);
        }

        void insert(const BSONObj& obj) {
            Client::WriteContext ctx(ns());
            _client.insert(ns(), obj);
        }

        IndexDescriptor* getIndex(const BSONObj& obj) {
            Client::ReadContext ctx(ns());
            Collection* collection = ctx.ctx().db()->getCollection( ns() );
            return collection->getIndexCatalog()->findIndexByKeyPattern( obj );
        }

       static const char* ns() { return "unittests.QueryStageDistinct"; }

    private:
        static DBDirectClient _client;
    };

    DBDirectClient DistinctBase::_client;

    class QueryStageDistinctBasic : public DistinctBase {
    public:
        virtual ~QueryStageDistinctBasic() { }

        void run() {
            // Insert a ton of documents with a: 1
            for (size_t i = 0; i < 1000; ++i) {
                insert(BSON("a" << 1));
            }

            // Insert a ton of other documents with a: 2
            for (size_t i = 0; i < 1000; ++i) {
                insert(BSON("a" << 2));
            }

            // Make an index on a:1
            addIndex(BSON("a" << 1));

            Client::ReadContext ctx(ns());

            // Set up the distinct stage.
            DistinctParams params;
            params.descriptor = getIndex(BSON("a" << 1));
            verify(params.descriptor);
            params.direction = 1;
            // Distinct-ing over the 0-th field of the keypattern.
            params.fieldNo = 0;
            // We'll look at all values in the bounds.
            params.bounds.isSimpleRange = false;
            OrderedIntervalList oil("a");
            oil.intervals.push_back(IndexBoundsBuilder::allValues());
            params.bounds.fields.push_back(oil);

            WorkingSet ws;
            DistinctScan* distinct = new DistinctScan(params, &ws);

            WorkingSetID wsid;
            // Get our first result.
            int firstResultWorks = 0;
            while (PlanStage::ADVANCED != distinct->work(&wsid)) {
                ++firstResultWorks;
            }
            // 5 is a bogus number.  There's some amount of setup done by the first few calls but
            // we should return the first result relatively promptly.
            ASSERT_LESS_THAN(firstResultWorks, 5);
            ASSERT_EQUALS(1, ws.get(wsid)->loc.obj()["a"].numberInt());

            // Getting our second result should be very quick as we just skip
            // over the first result.
            int secondResultWorks = 0;
            while (PlanStage::ADVANCED != distinct->work(&wsid)) {
                ++secondResultWorks;
            }
            ASSERT_EQUALS(2, ws.get(wsid)->loc.obj()["a"].numberInt());
            // This is 0 because we don't have to loop for several values; we just skip over
            // all the 'a' values.
            ASSERT_EQUALS(0, secondResultWorks);

            ASSERT_EQUALS(PlanStage::IS_EOF, distinct->work(&wsid));
        }
    };

    // XXX: add a test case with bounds where skipping to the next key gets us a result that's not
    // valid w.r.t. our query.

    class All : public Suite {
    public:
        All() : Suite( "query_stage_distinct" ) { }

        void setupTests() {
            add<QueryStageDistinctBasic>();
        }
    }  queryStageDistinctAll;

}  // namespace QueryStageDistinct
