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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include <boost/scoped_ptr.hpp>

#include "mongo/bson/bsonobj.h"
#include "mongo/client/dbclientcursor.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/exec/index_scan.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/query/stage_types.h"
#include "mongo/dbtests/dbtests.h"

namespace QueryStageIxscan {

    class IndexScanTest {
    public:
        IndexScanTest() :
            _ctx(ns()) { }

        virtual ~IndexScanTest() { }

        virtual void setup() {
            _client.dropCollection(ns());
            _client.ensureIndex(ns(), BSON("x" << 1));
        }

        void insert(const BSONObj& doc) {
            _client.insert(ns(), doc);
        }

        IndexScan* createIndexScan(BSONObj startKey, BSONObj endKey) {
            Collection* collection = _ctx.ctx().db()->getCollection(ns());
            IndexCatalog* catalog = collection->getIndexCatalog();
            IndexDescriptor* descriptor = catalog->findIndexByKeyPattern(BSON("x" << 1));
            invariant(descriptor);

            // We are not testing indexing here so use maximal bounds
            IndexScanParams params;
            params.descriptor = descriptor;
            params.bounds.isSimpleRange = true;
            params.bounds.startKey = startKey;
            params.bounds.endKey = endKey;
            params.bounds.endKeyInclusive = true;
            params.direction = 1;

            // This child stage gets owned and freed by the caller.
            MatchExpression* filter = NULL;
            return new IndexScan(params, &_ws, filter);
        }

        static const char* ns() { return "unittest.QueryStageIxscan"; }

    protected:
        WorkingSet _ws;
        Client::WriteContext _ctx;

        static DBDirectClient _client;
    };

    DBDirectClient IndexScanTest::_client;

    // SERVER-15958: Some IndexScanStats info must be initialized on construction of an IndexScan.
    class QueryStageIxscanInitializeStats : public IndexScanTest {
    public:
        void run() {
            setup();

            // Make the {x: 1} index multikey by inserting a doc where 'x' is an array.
            insert(fromjson("{_id: 1, x: [1, 2, 3]}"));

            std::auto_ptr<IndexScan> ixscan(createIndexScan(BSON("x" << 1), BSON("x" << 3)));

            // Verify that SpecificStats of 'ixscan' have been properly initialized.
            boost::scoped_ptr<PlanStageStats> stats(ixscan->getStats());
            ASSERT(stats.get());
            ASSERT_EQUALS(STAGE_IXSCAN, stats->stageType);

            const IndexScanStats* ixstats =
                static_cast<const IndexScanStats*>(stats->specific.get());
            ASSERT_TRUE(ixstats->isMultiKey);
            ASSERT_EQUALS(ixstats->keyPattern, BSON("x" << 1));
        }
    };

    class All : public Suite {
    public:
        All() : Suite("query_stage_ixscan") {}

        void setupTests() {
            add<QueryStageIxscanInitializeStats>();
        }
    } QueryStageIxscanAll;

} // namespace QueryStageIxscan
