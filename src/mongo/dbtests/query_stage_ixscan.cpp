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

#include "mongo/db/client.h"
#include "mongo/db/exec/index_scan.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/dbtests/dbtests.h"

namespace QueryStageIxscan {

    class IndexScanTest {
    public:
        IndexScanTest() :
            _dbLock(_txn.lockState(), nsToDatabaseSubstring(ns()), MODE_X),
            _ctx(&_txn, ns()),
            _coll(NULL) { }

        virtual ~IndexScanTest() { }

        virtual void setup() {
            WriteUnitOfWork wunit(&_txn);

            _ctx.db()->dropCollection(&_txn, ns());
            _coll = _ctx.db()->createCollection(&_txn, ns());

            ASSERT_OK(dbtests::createIndex(&_txn, ns(), BSON("x" << 1)));

            wunit.commit();
        }

        void insert(const BSONObj& doc) {
            WriteUnitOfWork wunit(&_txn);
            _coll->insertDocument(&_txn, doc, false);
            wunit.commit();
        }

        IndexScan* createIndexScan(BSONObj startKey, BSONObj endKey) {
            IndexCatalog* catalog = _coll->getIndexCatalog();
            IndexDescriptor* descriptor = catalog->findIndexByKeyPattern(&_txn, BSON("x" << 1));
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
            return new IndexScan(&_txn, params, &_ws, filter);
        }

        static const char* ns() { return "unittest.QueryStageIxscan"; }

    protected:
        OperationContextImpl _txn;
        WorkingSet _ws;

        Lock::DBLock _dbLock;
        Client::Context _ctx;
        Collection* _coll;
    };

    // SERVER-15958: Some IndexScanStats info must be initialized on construction of an IndexScan.
    class QueryStageIxscanInitializeStats : public IndexScanTest {
    public:
        void run() {
            setup();

            // Make the {x: 1} index multikey by inserting a doc where 'x' is an array.
            insert(fromjson("{_id: 1, x: [1, 2, 3]}"));

            std::auto_ptr<IndexScan> ixscan(createIndexScan(BSON("x" << 1), BSON("x" << 3)));

            // Verify that SpecificStats of 'ixscan' have been properly initialized.
            const IndexScanStats* stats =
                static_cast<const IndexScanStats*>(ixscan->getSpecificStats());
            ASSERT(stats);
            ASSERT_TRUE(stats->isMultiKey);
            ASSERT_EQUALS(stats->keyPattern, BSON("x" << 1));
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
