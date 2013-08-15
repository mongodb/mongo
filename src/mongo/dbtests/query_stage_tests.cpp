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

#include "mongo/client/dbclientcursor.h"
#include "mongo/db/exec/index_scan.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/index/catalog_hack.h"
#include "mongo/db/instance.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/dbtests/dbtests.h"

/**
 * This file tests db/exec/index_scan.cpp
 */

namespace QueryStageTests {

    class IndexScanBase {
    public:
        IndexScanBase() {
            Client::WriteContext ctx(ns());

            for (int i = 0; i < numObj(); ++i) {
                BSONObjBuilder bob;
                bob.append("foo", i);
                bob.append("baz", i);
                bob.append("bar", numObj() - i);
                _client.insert(ns(), bob.obj());
            }

            addIndex(BSON("foo" << 1));
            addIndex(BSON("foo" << 1 << "baz" << 1));
        }

        virtual ~IndexScanBase() {
            Client::WriteContext ctx(ns());
            _client.dropCollection(ns());
        }

        void addIndex(const BSONObj& obj) {
            Client::WriteContext ctx(ns());
            _client.ensureIndex(ns(), obj);
        }

        int countResults(const IndexScanParams& params, BSONObj filterObj = BSONObj()) {
            Client::ReadContext ctx(ns());

            StatusWithMatchExpression swme = MatchExpressionParser::parse(filterObj);
            verify(swme.isOK());
            auto_ptr<MatchExpression> filterExpr(swme.getValue());

            WorkingSet* ws = new WorkingSet();
            PlanExecutor runner(ws, new IndexScan(params, ws, filterExpr.get()));

            int count = 0;
            for (DiskLoc dl; Runner::RUNNER_ADVANCED == runner.getNext(NULL, &dl); ) {
                ++count;
            }

            return count;
        }

        void makeGeoData() {
            Client::WriteContext ctx(ns());

            for (int i = 0; i < numObj(); ++i) {
                double lat = double(rand()) / RAND_MAX;
                double lng = double(rand()) / RAND_MAX;
                _client.insert(ns(), BSON("geo" << BSON_ARRAY(lng << lat)));
            }
        }

        IndexDescriptor* getIndex(const BSONObj& obj) {
            Client::ReadContext ctx(ns());
            NamespaceDetails* nsd = nsdetails(ns());
            int idxNo = nsd->findIndexByKeyPattern(obj);
            return CatalogHack::getDescriptor(nsd, idxNo);
        }

        static int numObj() { return 50; }
        static const char* ns() { return "unittests.IndexScan"; }

    private:
        static DBDirectClient _client;
    };

    DBDirectClient IndexScanBase::_client;

    class QueryStageIXScanBasic : public IndexScanBase {
    public:
        virtual ~QueryStageIXScanBasic() { }

        void run() {
            // foo <= 20
            IndexScanParams params;
            params.descriptor = getIndex(BSON("foo" << 1));
            params.bounds.isSimpleRange = true;
            params.bounds.startKey = BSON("" << 20);
            params.bounds.endKey = BSONObj();
            params.bounds.endKeyInclusive = true;
            params.direction = -1;

            ASSERT_EQUALS(countResults(params), 21);
        }
    };

    class QueryStageIXScanLowerUpper : public IndexScanBase {
    public:
        virtual ~QueryStageIXScanLowerUpper() { }

        void run() {
            // 20 <= foo < 30
            IndexScanParams params;
            params.descriptor = getIndex(BSON("foo" << 1));
            params.bounds.isSimpleRange = true;
            params.bounds.startKey = BSON("" << 20);
            params.bounds.endKey = BSON("" << 30);
            params.bounds.endKeyInclusive = false;
            params.direction = 1;

            ASSERT_EQUALS(countResults(params), 10);
        }
    };

    class QueryStageIXScanLowerUpperIncl : public IndexScanBase {
    public:
        virtual ~QueryStageIXScanLowerUpperIncl() { }

        void run() {
            // 20 <= foo <= 30
            IndexScanParams params;
            params.descriptor = getIndex(BSON("foo" << 1));
            params.bounds.isSimpleRange = true;
            params.bounds.startKey = BSON("" << 20);
            params.bounds.endKey = BSON("" << 30);
            params.bounds.endKeyInclusive = true;
            params.direction = 1;

            ASSERT_EQUALS(countResults(params), 11);
        }
    };

    class QueryStageIXScanLowerUpperInclFilter : public IndexScanBase {
    public:
        virtual ~QueryStageIXScanLowerUpperInclFilter() { }

        void run() {
            // 20 <= foo < 30
            // foo == 25
            IndexScanParams params;
            params.descriptor = getIndex(BSON("foo" << 1));
            params.bounds.isSimpleRange = true;
            params.bounds.startKey = BSON("" << 20);
            params.bounds.endKey = BSON("" << 30);
            params.bounds.endKeyInclusive = true;
            params.direction = 1;

            ASSERT_EQUALS(countResults(params, BSON("foo" << 25)), 1);
        }
    };

    class QueryStageIXScanCantMatch : public IndexScanBase {
    public:
        virtual ~QueryStageIXScanCantMatch() { }

        void run() {
            // 20 <= foo < 30
            // bar == 25 (not covered, should error.)
            IndexScanParams params;
            params.descriptor = getIndex(BSON("foo" << 1));
            params.bounds.isSimpleRange = true;
            params.bounds.startKey = BSON("" << 20);
            params.bounds.endKey = BSON("" << 30);
            params.bounds.endKeyInclusive = true;
            params.direction = 1;

            ASSERT_THROWS(countResults(params, BSON("baz" << 25)), MsgAssertionException);
        }
    };

    class QueryStageIXScan2dSphere : public IndexScanBase {
    public:
        virtual ~QueryStageIXScan2dSphere() { }

        void run() {
            // Add numObj() geo points.  Make sure we get them back.
            makeGeoData();
            addIndex(BSON("geo" << "2dsphere"));

            IndexScanParams params;
            params.descriptor = getIndex(BSON("geo" << "2dsphere"));
            params.bounds.isSimpleRange = true;
            params.bounds.startKey = BSON("geo" << BSON("$geoNear" << BSON("$geometry"
                                  << BSON("type" << "Point"
                                        << "coordinates" << BSON_ARRAY(0 << 0)))));
            params.bounds.endKey = BSONObj();
            params.bounds.endKeyInclusive = true;
            params.direction = 1;

            ASSERT_EQUALS(countResults(params), numObj());
        }
    };

    class QueryStageIXScan2d : public IndexScanBase {
    public:
        virtual ~QueryStageIXScan2d() { }

        void run() {
            makeGeoData();
            addIndex(BSON("geo" << "2d"));

            // 2d should also work.
            IndexScanParams params;
            params.descriptor = getIndex(BSON("geo" << "2d"));
            params.bounds.isSimpleRange = true;
            params.bounds.startKey = BSON("geo" << BSON("$near" << BSON_ARRAY(0 << 0)));
            params.bounds.endKey = BSONObj();
            params.bounds.endKeyInclusive = true;
            params.direction = 1;

            ASSERT_EQUALS(countResults(params), numObj());
        }
    };

    class All : public Suite {
    public:
        All() : Suite( "query_stage_tests" ) { }

        void setupTests() {
            add<QueryStageIXScanBasic>();
            add<QueryStageIXScanLowerUpper>();
            add<QueryStageIXScanLowerUpperIncl>();
            add<QueryStageIXScanLowerUpperInclFilter>();
            add<QueryStageIXScanCantMatch>();
            add<QueryStageIXScan2dSphere>();
            add<QueryStageIXScan2d>();
        }
    }  queryStageTestsAll;

}  // namespace
