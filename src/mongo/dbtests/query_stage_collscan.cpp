/**
 *    Copyright (C) 2013-2014 MongoDB Inc.
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

/**
 * This file tests db/exec/collection_scan.cpp.
 */

#include "mongo/client/dbclientcursor.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/instance.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/storage/mmap_v1/extent.h"
#include "mongo/db/storage/mmap_v1/extent_manager.h"
#include "mongo/db/storage/mmap_v1/mmap_v1_extent_manager.h"
// #include "mongo/db/structure/catalog/namespace_details.h"  // XXX SERVER-13640
#include "mongo/db/storage/record_store.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/util/fail_point_service.h"

namespace QueryStageCollectionScan {

    //
    // Stage-specific tests.
    //

    class QueryStageCollectionScanBase {
    public:
        QueryStageCollectionScanBase() : _client(&_txn) {
            Client::WriteContext ctx(&_txn, ns());

            for (int i = 0; i < numObj(); ++i) {
                BSONObjBuilder bob;
                bob.append("foo", i);
                _client.insert(ns(), bob.obj());
            }
            ctx.commit();
        }

        virtual ~QueryStageCollectionScanBase() {
            Client::WriteContext ctx(&_txn, ns());
            _client.dropCollection(ns());
            ctx.commit();
        }

        void remove(const BSONObj& obj) {
            _client.remove(ns(), obj);
        }

        int countResults(CollectionScanParams::Direction direction, const BSONObj& filterObj) {
            Client::ReadContext ctx(&_txn, ns());

            // Configure the scan.
            CollectionScanParams params;
            params.collection = ctx.ctx().db()->getCollection( &_txn, ns() );
            params.direction = direction;
            params.tailable = false;

            // Make the filter.
            StatusWithMatchExpression swme = MatchExpressionParser::parse(filterObj);
            verify(swme.isOK());
            auto_ptr<MatchExpression> filterExpr(swme.getValue());

            // Make a scan and have the runner own it.
            WorkingSet* ws = new WorkingSet();
            PlanStage* ps = new CollectionScan(&_txn, params, ws, filterExpr.get());
            PlanExecutor runner(ws, ps, params.collection);

            // Use the runner to count the number of objects scanned.
            int count = 0;
            for (BSONObj obj; PlanExecutor::ADVANCED == runner.getNext(&obj, NULL); ) { ++count; }
            return count;
        }

        void getLocs(Collection* collection,
                     CollectionScanParams::Direction direction,
                     vector<DiskLoc>* out) {
            WorkingSet ws;

            CollectionScanParams params;
            params.collection = collection;
            params.direction = direction;
            params.tailable = false;

            scoped_ptr<CollectionScan> scan(new CollectionScan(&_txn, params, &ws, NULL));
            while (!scan->isEOF()) {
                WorkingSetID id = WorkingSet::INVALID_ID;
                PlanStage::StageState state = scan->work(&id);
                if (PlanStage::ADVANCED == state) {
                    WorkingSetMember* member = ws.get(id);
                    verify(member->hasLoc());
                    out->push_back(member->loc);
                }
            }
        }

        static int numObj() { return 50; }

        static const char* ns() { return "unittests.QueryStageCollectionScan"; }

    protected:
        OperationContextImpl _txn;

    private:
        DBDirectClient _client;
    };


    //
    // Go forwards, get everything.
    //
    class QueryStageCollscanBasicForward : public QueryStageCollectionScanBase {
    public:
        void run() {
            ASSERT_EQUALS(numObj(), countResults(CollectionScanParams::FORWARD, BSONObj()));
        }
    };

    //
    // Go backwards, get everything.
    //

    class QueryStageCollscanBasicBackward : public QueryStageCollectionScanBase {
    public:
        void run() {
            ASSERT_EQUALS(numObj(), countResults(CollectionScanParams::BACKWARD, BSONObj()));
        }
    };

    //
    // Go forwards and match half the docs.
    //

    class QueryStageCollscanBasicForwardWithMatch : public QueryStageCollectionScanBase {
    public:
        void run() {
            BSONObj obj = BSON("foo" << BSON("$lt" << 25));
            ASSERT_EQUALS(25, countResults(CollectionScanParams::FORWARD, obj));
        }
    };

    //
    // Go backwards and match half the docs.
    //

    class QueryStageCollscanBasicBackwardWithMatch : public QueryStageCollectionScanBase {
    public:
        void run() {
            BSONObj obj = BSON("foo" << BSON("$lt" << 25));
            ASSERT_EQUALS(25, countResults(CollectionScanParams::BACKWARD, obj));
        }
    };

    //
    // Get objects in the order we inserted them.
    //

    class QueryStageCollscanObjectsInOrderForward : public QueryStageCollectionScanBase {
    public:
        void run() {
            Client::ReadContext ctx(&_txn, ns());

            // Configure the scan.
            CollectionScanParams params;
            params.collection = ctx.ctx().db()->getCollection( &_txn, ns() );
            params.direction = CollectionScanParams::FORWARD;
            params.tailable = false;

            // Make a scan and have the runner own it.
            WorkingSet* ws = new WorkingSet();
            PlanStage* ps = new CollectionScan(&_txn, params, ws, NULL);
            PlanExecutor runner(ws, ps, params.collection);

            int count = 0;
            for (BSONObj obj; PlanExecutor::ADVANCED == runner.getNext(&obj, NULL); ) {
                // Make sure we get the objects in the order we want
                ASSERT_EQUALS(count, obj["foo"].numberInt());
                ++count;
            }

            ASSERT_EQUALS(numObj(), count);
        }
    };

    //
    // Get objects in the reverse order we inserted them when we go backwards.
    //

    class QueryStageCollscanObjectsInOrderBackward : public QueryStageCollectionScanBase {
    public:
        void run() {
            Client::ReadContext ctx(&_txn, ns());

            CollectionScanParams params;
            params.collection = ctx.ctx().db()->getCollection( &_txn, ns() );
            params.direction = CollectionScanParams::BACKWARD;
            params.tailable = false;

            WorkingSet* ws = new WorkingSet();
            PlanStage* ps = new CollectionScan(&_txn, params, ws, NULL);
            PlanExecutor runner(ws, ps, params.collection);

            int count = 0;
            for (BSONObj obj; PlanExecutor::ADVANCED == runner.getNext(&obj, NULL); ) {
                ++count;
                ASSERT_EQUALS(numObj() - count, obj["foo"].numberInt());
            }

            ASSERT_EQUALS(numObj(), count);
        }
    };

    //
    // Scan through half the objects, delete the one we're about to fetch, then expect to get the
    // "next" object we would have gotten after that.
    //

    class QueryStageCollscanInvalidateUpcomingObject : public QueryStageCollectionScanBase {
    public:
        void run() {
            Client::WriteContext ctx(&_txn, ns());

            Collection* coll = ctx.ctx().db()->getCollection( &_txn, ns() );

            // Get the DiskLocs that would be returned by an in-order scan.
            vector<DiskLoc> locs;
            getLocs(coll, CollectionScanParams::FORWARD, &locs);

            // Configure the scan.
            CollectionScanParams params;
            params.collection = coll;
            params.direction = CollectionScanParams::FORWARD;
            params.tailable = false;

            WorkingSet ws;
            scoped_ptr<CollectionScan> scan(new CollectionScan(&_txn, params, &ws, NULL));

            int count = 0;
            while (count < 10) {
                WorkingSetID id = WorkingSet::INVALID_ID;
                PlanStage::StageState state = scan->work(&id);
                if (PlanStage::ADVANCED == state) {
                    WorkingSetMember* member = ws.get(id);
                    ASSERT_EQUALS(coll->docFor(&_txn, locs[count])["foo"].numberInt(),
                                  member->obj["foo"].numberInt());
                    ++count;
                }
            }

            // Remove locs[count].
            scan->saveState();
            scan->invalidate(locs[count], INVALIDATION_DELETION);
            remove(coll->docFor(&_txn, locs[count]));
            scan->restoreState(&_txn);

            // Skip over locs[count].
            ++count;

            // Expect the rest.
            while (!scan->isEOF()) {
                WorkingSetID id = WorkingSet::INVALID_ID;
                PlanStage::StageState state = scan->work(&id);
                if (PlanStage::ADVANCED == state) {
                    WorkingSetMember* member = ws.get(id);
                    ASSERT_EQUALS(coll->docFor(&_txn, locs[count])["foo"].numberInt(),
                                  member->obj["foo"].numberInt());
                    ++count;
                }
            }
            ctx.commit();

            ASSERT_EQUALS(numObj(), count);
        }
    };

    //
    // Scan through half the objects, delete the one we're about to fetch, then expect to get the
    // "next" object we would have gotten after that.  But, do it in reverse!
    //

    class QueryStageCollscanInvalidateUpcomingObjectBackward : public QueryStageCollectionScanBase {
    public:
        void run() {
            Client::WriteContext ctx(&_txn, ns());
            Collection* coll = ctx.ctx().db()->getCollection(&_txn, ns());

            // Get the DiskLocs that would be returned by an in-order scan.
            vector<DiskLoc> locs;
            getLocs(coll, CollectionScanParams::BACKWARD, &locs);

            // Configure the scan.
            CollectionScanParams params;
            params.collection = coll;
            params.direction = CollectionScanParams::BACKWARD;
            params.tailable = false;

            WorkingSet ws;
            scoped_ptr<CollectionScan> scan(new CollectionScan(&_txn, params, &ws, NULL));

            int count = 0;
            while (count < 10) {
                WorkingSetID id = WorkingSet::INVALID_ID;
                PlanStage::StageState state = scan->work(&id);
                if (PlanStage::ADVANCED == state) {
                    WorkingSetMember* member = ws.get(id);
                    ASSERT_EQUALS(coll->docFor(&_txn, locs[count])["foo"].numberInt(),
                                  member->obj["foo"].numberInt());
                    ++count;
                }
            }

            // Remove locs[count].
            scan->saveState();
            scan->invalidate(locs[count], INVALIDATION_DELETION);
            remove(coll->docFor(&_txn, locs[count]));
            scan->restoreState(&_txn);

            // Skip over locs[count].
            ++count;

            // Expect the rest.
            while (!scan->isEOF()) {
                WorkingSetID id = WorkingSet::INVALID_ID;
                PlanStage::StageState state = scan->work(&id);
                if (PlanStage::ADVANCED == state) {
                    WorkingSetMember* member = ws.get(id);
                    ASSERT_EQUALS(coll->docFor(&_txn, locs[count])["foo"].numberInt(),
                                  member->obj["foo"].numberInt());
                    ++count;
                }
            }
            ctx.commit();

            ASSERT_EQUALS(numObj(), count);
        }
    };

    class All : public Suite {
    public:
        All() : Suite( "QueryStageCollectionScan" ) {}

        void setupTests() {
            // Stage-specific tests below.
            add<QueryStageCollscanBasicForward>();
            add<QueryStageCollscanBasicBackward>();
            add<QueryStageCollscanBasicForwardWithMatch>();
            add<QueryStageCollscanBasicBackwardWithMatch>();
            add<QueryStageCollscanObjectsInOrderForward>();
            add<QueryStageCollscanObjectsInOrderBackward>();
            add<QueryStageCollscanInvalidateUpcomingObject>();
            add<QueryStageCollscanInvalidateUpcomingObjectBackward>();
        }
    } all;

}
