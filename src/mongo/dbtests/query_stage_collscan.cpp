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

/**
 * This file tests db/exec/collection_scan.cpp.
 */

#include "mongo/client/dbclientcursor.h"
#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/instance.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/dbtests/dbtests.h"

namespace QueryStageCollectionScan {

    //
    // Test some nitty-gritty capped collection details.  Ported and polished from pdfiletests.cpp.
    //
    class QueryStageCollectionScanCappedBase {
    public:
        QueryStageCollectionScanCappedBase() : _context(ns()) { }

        virtual ~QueryStageCollectionScanCappedBase() {
            dropNS(ns());
        }

        void run() {
            // Create the capped collection.
            stringstream spec;
            spec << "{\"capped\":true,\"size\":2000,\"$nExtents\":" << nExtents() << "}";

            string err;
            ASSERT( userCreateNS( ns(), fromjson( spec.str() ), err, false ) );

            // Tell the test to add data/extents/etc.
            insertTestData();

            CollectionScanParams params;
            params.ns = ns();
            params.direction = CollectionScanParams::FORWARD;
            params.tailable = false;
            params.start = DiskLoc();

            // Walk the collection going forward.
            {
                // Create an executor to handle the scan.
                WorkingSet* ws = new WorkingSet();
                PlanStage* ps = new CollectionScan(params, ws, NULL);
                PlanExecutor runner(ws, ps);

                int resultCount = 0;
                BSONObj obj;
                while (Runner::RUNNER_ADVANCED == runner.getNext(&obj, NULL)) {
                    ASSERT_EQUALS(resultCount, obj.firstElement().number());
                    ++resultCount;
                }

                ASSERT_EQUALS(expectedCount(), resultCount);
            }

            // Walk the collection going backwards.
            {
                params.direction = CollectionScanParams::BACKWARD;

                WorkingSet* ws = new WorkingSet();
                PlanStage* ps = new CollectionScan(params, ws, NULL);
                PlanExecutor runner(ws, ps);

                // Going backwards.
                int resultCount = expectedCount() - 1;
                BSONObj obj;
                while (Runner::RUNNER_ADVANCED == runner.getNext(&obj, NULL)) {
                    ASSERT_EQUALS(resultCount, obj.firstElement().number());
                    --resultCount;
                }

                ASSERT_EQUALS(-1, resultCount);
            }
        }

    protected:
        // Insert records into the collection.
        virtual void insertTestData() = 0;

        // How many records do we expect to find in our scan?
        virtual int expectedCount() const = 0;

        // How many extents do we create when we make the collection?
        virtual int nExtents() const = 0;

        // Quote: bypass standard alloc/insert routines to use the extent we want.
        static DiskLoc insert( const DiskLoc& ext, int i ) {
            // Copied verbatim.
            BSONObjBuilder b;
            b.append( "a", i );
            BSONObj o = b.done();
            int len = o.objsize();
            Extent *e = ext.ext();
            e = getDur().writing(e);
            int ofs;
            if ( e->lastRecord.isNull() )
                ofs = ext.getOfs() + ( e->_extentData - (char *)e );
            else
                ofs = e->lastRecord.getOfs() + e->lastRecord.rec()->lengthWithHeaders();
            DiskLoc dl( ext.a(), ofs );
            Record *r = dl.rec();
            r = (Record*) getDur().writingPtr(r, Record::HeaderSize + len);
            r->lengthWithHeaders() = Record::HeaderSize + len;
            r->extentOfs() = e->myLoc.getOfs();
            r->nextOfs() = DiskLoc::NullOfs;
            r->prevOfs() = e->lastRecord.isNull() ? DiskLoc::NullOfs : e->lastRecord.getOfs();
            memcpy( r->data(), o.objdata(), len );
            if ( e->firstRecord.isNull() )
                e->firstRecord = dl;
            else
                getDur().writingInt(e->lastRecord.rec()->nextOfs()) = ofs;
            e->lastRecord = dl;
            return dl;
        }

        static const char *ns() { return "unittests.QueryStageCollectionScanCapped"; }

        static NamespaceDetails *nsd() { return nsdetails(ns()); }

    private:
        Lock::GlobalWrite lk_;
        Client::Context _context;
    };

    class QueryStageCollscanEmpty : public QueryStageCollectionScanCappedBase {
        virtual void insertTestData() {}
        virtual int expectedCount() const { return 0; }
        virtual int nExtents() const { return 0; }
    };

    class QueryStageCollscanEmptyLooped : public QueryStageCollectionScanCappedBase {
        virtual void insertTestData() {
            nsd()->writingWithExtra()->capFirstNewRecord() = DiskLoc();
        }
        virtual int expectedCount() const { return 0; }
        virtual int nExtents() const { return 0; }
    };

    class QueryStageCollscanEmptyMultiExtentLooped : public QueryStageCollectionScanCappedBase {
        virtual void insertTestData() {
            nsd()->writingWithExtra()->capFirstNewRecord() = DiskLoc();
        }
        virtual int expectedCount() const { return 0; }
        virtual int nExtents() const { return 3; }
    };

    class QueryStageCollscanSingle : public QueryStageCollectionScanCappedBase {
        virtual void insertTestData() {
            nsd()->writingWithExtra()->capFirstNewRecord() = insert( nsd()->capExtent(), 0 );
        }
        virtual int expectedCount() const { return 1; }
        virtual int nExtents() const { return 0; }
    };

    class QueryStageCollscanNewCapFirst : public QueryStageCollectionScanCappedBase {
        virtual void insertTestData() {
            DiskLoc x = insert( nsd()->capExtent(), 0 );
            nsd()->writingWithExtra()->capFirstNewRecord() = x;
            insert( nsd()->capExtent(), 1 );
        }
        virtual int expectedCount() const { return 2; }
        virtual int nExtents() const { return 0; }
    };

    class QueryStageCollscanNewCapLast : public QueryStageCollectionScanCappedBase {
        virtual void insertTestData() {
            insert( nsd()->capExtent(), 0 );
            nsd()->capFirstNewRecord().writing() = insert( nsd()->capExtent(), 1 );
        }
        virtual int expectedCount() const { return 2; }
        virtual int nExtents() const { return 0; }
    };

    class QueryStageCollscanNewCapMiddle : public QueryStageCollectionScanCappedBase {
        virtual void insertTestData() {
            insert( nsd()->capExtent(), 0 );
            nsd()->capFirstNewRecord().writing() = insert( nsd()->capExtent(), 1 );
            insert( nsd()->capExtent(), 2 );
        }
        virtual int expectedCount() const { return 3; }
        virtual int nExtents() const { return 0; }
    };

    class QueryStageCollscanFirstExtent : public QueryStageCollectionScanCappedBase {
        virtual void insertTestData() {
            insert( nsd()->capExtent(), 0 );
            insert( nsd()->lastExtent(), 1 );
            nsd()->capFirstNewRecord().writing() = insert( nsd()->capExtent(), 2 );
            insert( nsd()->capExtent(), 3 );
        }
        virtual int expectedCount() const { return 4; }
        virtual int nExtents() const { return 2; }
    };

    class QueryStageCollscanLastExtent : public QueryStageCollectionScanCappedBase {
        virtual void insertTestData() {
            nsd()->capExtent().writing() = nsd()->lastExtent();
            insert( nsd()->capExtent(), 0 );
            insert( nsd()->firstExtent(), 1 );
            nsd()->capFirstNewRecord().writing() = insert( nsd()->capExtent(), 2 );
            insert( nsd()->capExtent(), 3 );
        }
        virtual int expectedCount() const { return 4; }
        virtual int nExtents() const { return 2; }
    };

    class QueryStageCollscanMidExtent : public QueryStageCollectionScanCappedBase {
        virtual void insertTestData() {
            nsd()->capExtent().writing() = nsd()->firstExtent().ext()->xnext;
            insert( nsd()->capExtent(), 0 );
            insert( nsd()->lastExtent(), 1 );
            insert( nsd()->firstExtent(), 2 );
            nsd()->capFirstNewRecord().writing() = insert( nsd()->capExtent(), 3 );
            insert( nsd()->capExtent(), 4 );
        }
        virtual int expectedCount() const { return 5; }
        virtual int nExtents() const { return 3; }
    };

    class QueryStageCollscanAloneInExtent : public QueryStageCollectionScanCappedBase {
        virtual void insertTestData() {
            nsd()->capExtent().writing() = nsd()->firstExtent().ext()->xnext;
            insert( nsd()->lastExtent(), 0 );
            insert( nsd()->firstExtent(), 1 );
            nsd()->capFirstNewRecord().writing() = insert( nsd()->capExtent(), 2 );
        }
        virtual int expectedCount() const { return 3; }
        virtual int nExtents() const { return 3; }
    };

    class QueryStageCollscanFirstInExtent : public QueryStageCollectionScanCappedBase {
        virtual void insertTestData() {
            nsd()->capExtent().writing() = nsd()->firstExtent().ext()->xnext;
            insert( nsd()->lastExtent(), 0 );
            insert( nsd()->firstExtent(), 1 );
            nsd()->capFirstNewRecord().writing() = insert( nsd()->capExtent(), 2 );
            insert( nsd()->capExtent(), 3 );
        }
        virtual int expectedCount() const { return 4; }
        virtual int nExtents() const { return 3; }
    };

    class QueryStageCollscanLastInExtent : public QueryStageCollectionScanCappedBase {
        virtual void insertTestData() {
            nsd()->capExtent().writing() = nsd()->firstExtent().ext()->xnext;
            insert( nsd()->capExtent(), 0 );
            insert( nsd()->lastExtent(), 1 );
            insert( nsd()->firstExtent(), 2 );
            nsd()->capFirstNewRecord().writing() = insert( nsd()->capExtent(), 3 );
        }
        virtual int expectedCount() const { return 4; }
        virtual int nExtents() const { return 3; }
    };

    //
    // Stage-specific tests.
    //

    class QueryStageCollectionScanBase {
    public:
        QueryStageCollectionScanBase() {
            Client::WriteContext ctx(ns());

            for (int i = 0; i < numObj(); ++i) {
                BSONObjBuilder bob;
                bob.append("foo", i);
                _client.insert(ns(), bob.obj());
            }
        }

        virtual ~QueryStageCollectionScanBase() {
            Client::WriteContext ctx(ns());
            _client.dropCollection(ns());
        }

        void remove(const BSONObj& obj) {
            _client.remove(ns(), obj);
        }

        int countResults(CollectionScanParams::Direction direction, const BSONObj& filterObj) {
            Client::ReadContext ctx(ns());

            // Configure the scan.
            CollectionScanParams params;
            params.ns = ns();
            params.direction = direction;
            params.tailable = false;

            // Make the filter.
            StatusWithMatchExpression swme = MatchExpressionParser::parse(filterObj);
            verify(swme.isOK());
            auto_ptr<MatchExpression> filterExpr(swme.getValue());

            // Make a scan and have the runner own it.
            WorkingSet* ws = new WorkingSet();
            PlanStage* ps = new CollectionScan(params, ws, filterExpr.get());
            PlanExecutor runner(ws, ps);

            // Use the runner to count the number of objects scanned.
            int count = 0;
            for (BSONObj obj; Runner::RUNNER_ADVANCED == runner.getNext(&obj, NULL); ) { ++count; }
            return count;
        }

        void getLocs(CollectionScanParams::Direction direction, vector<DiskLoc>* out) {
            WorkingSet ws;

            CollectionScanParams params;
            params.ns = ns();
            params.direction = direction;
            params.tailable = false;

            scoped_ptr<CollectionScan> scan(new CollectionScan(params, &ws, NULL));
            while (!scan->isEOF()) {
                WorkingSetID id;
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

    private:
        static DBDirectClient _client;
    };

    DBDirectClient QueryStageCollectionScanBase::_client;

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
            Client::ReadContext ctx(ns());

            // Configure the scan.
            CollectionScanParams params;
            params.ns = ns();
            params.direction = CollectionScanParams::FORWARD;
            params.tailable = false;

            // Make a scan and have the runner own it.
            WorkingSet* ws = new WorkingSet();
            PlanStage* ps = new CollectionScan(params, ws, NULL);
            PlanExecutor runner(ws, ps);

            int count = 0;
            for (BSONObj obj; Runner::RUNNER_ADVANCED == runner.getNext(&obj, NULL); ) {
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
            Client::ReadContext ctx(ns());

            CollectionScanParams params;
            params.ns = ns();
            params.direction = CollectionScanParams::BACKWARD;
            params.tailable = false;

            WorkingSet* ws = new WorkingSet();
            PlanStage* ps = new CollectionScan(params, ws, NULL);
            PlanExecutor runner(ws, ps);

            int count = 0;
            for (BSONObj obj; Runner::RUNNER_ADVANCED == runner.getNext(&obj, NULL); ) {
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
            Client::WriteContext ctx(ns());

            // Get the DiskLocs that would be returned by an in-order scan.
            vector<DiskLoc> locs;
            getLocs(CollectionScanParams::FORWARD, &locs);

            // Configure the scan.
            CollectionScanParams params;
            params.ns = ns();
            params.direction = CollectionScanParams::FORWARD;
            params.tailable = false;

            WorkingSet ws;
            scoped_ptr<CollectionScan> scan(new CollectionScan(params, &ws, NULL));

            int count = 0;
            while (count < 10) {
                WorkingSetID id;
                PlanStage::StageState state = scan->work(&id);
                if (PlanStage::ADVANCED == state) {
                    WorkingSetMember* member = ws.get(id);
                    ASSERT_EQUALS(locs[count].obj()["foo"].numberInt(),
                                  member->obj["foo"].numberInt());
                    ++count;
                }
            }

            // Remove locs[count].
            scan->prepareToYield();
            scan->invalidate(locs[count]);
            remove(locs[count].obj());
            scan->recoverFromYield();

            // Skip over locs[count].
            ++count;

            // Expect the rest.
            while (!scan->isEOF()) {
                WorkingSetID id;
                PlanStage::StageState state = scan->work(&id);
                if (PlanStage::ADVANCED == state) {
                    WorkingSetMember* member = ws.get(id);
                    ASSERT_EQUALS(locs[count].obj()["foo"].numberInt(),
                                  member->obj["foo"].numberInt());
                    ++count;
                }
            }

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
            Client::WriteContext ctx(ns());

            // Get the DiskLocs that would be returned by an in-order scan.
            vector<DiskLoc> locs;
            getLocs(CollectionScanParams::BACKWARD, &locs);

            // Configure the scan.
            CollectionScanParams params;
            params.ns = ns();
            params.direction = CollectionScanParams::BACKWARD;
            params.tailable = false;

            WorkingSet ws;
            scoped_ptr<CollectionScan> scan(new CollectionScan(params, &ws, NULL));

            int count = 0;
            while (count < 10) {
                WorkingSetID id;
                PlanStage::StageState state = scan->work(&id);
                if (PlanStage::ADVANCED == state) {
                    WorkingSetMember* member = ws.get(id);
                    ASSERT_EQUALS(locs[count].obj()["foo"].numberInt(),
                                  member->obj["foo"].numberInt());
                    ++count;
                }
            }

            // Remove locs[count].
            scan->prepareToYield();
            scan->invalidate(locs[count]);
            remove(locs[count].obj());
            scan->recoverFromYield();

            // Skip over locs[count].
            ++count;

            // Expect the rest.
            while (!scan->isEOF()) {
                WorkingSetID id;
                PlanStage::StageState state = scan->work(&id);
                if (PlanStage::ADVANCED == state) {
                    WorkingSetMember* member = ws.get(id);
                    ASSERT_EQUALS(locs[count].obj()["foo"].numberInt(),
                                  member->obj["foo"].numberInt());
                    ++count;
                }
            }

            ASSERT_EQUALS(numObj(), count);
        }
    };

    class All : public Suite {
    public:
        All() : Suite( "QueryStageCollectionScan" ) {}

        void setupTests() {
            // These tests are ported from pdfile.cpp
            add<QueryStageCollscanEmpty>();
            add<QueryStageCollscanEmptyLooped>();
            add<QueryStageCollscanEmptyMultiExtentLooped>();
            add<QueryStageCollscanSingle>();
            add<QueryStageCollscanNewCapFirst>();
            add<QueryStageCollscanNewCapLast>();
            add<QueryStageCollscanNewCapMiddle>();
            add<QueryStageCollscanFirstExtent>();
            add<QueryStageCollscanLastExtent>();
            add<QueryStageCollscanMidExtent>();
            add<QueryStageCollscanAloneInExtent>();
            add<QueryStageCollscanFirstInExtent>();
            add<QueryStageCollscanLastInExtent>();
            // These are not.  Stage-specific tests below.
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
