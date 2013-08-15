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
#include "mongo/db/exec/fetch.h"
#include "mongo/db/exec/index_scan.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/merge_sort.h"
#include "mongo/db/index/catalog_hack.h"
#include "mongo/db/instance.h"
#include "mongo/db/json.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/dbtests/dbtests.h"

/**
 * This file tests db/exec/merge_sort.cpp
 */

namespace QueryStageMergeSortTests {

    class QueryStageMergeSortTestBase {
    public:
        QueryStageMergeSortTestBase() { }

        virtual ~QueryStageMergeSortTestBase() {
            Client::WriteContext ctx(ns());
            _client.dropCollection(ns());
        }

        void addIndex(const BSONObj& obj) {
            _client.ensureIndex(ns(), obj);
        }

        IndexDescriptor* getIndex(const BSONObj& obj) {
            NamespaceDetails* nsd = nsdetails(ns());
            int idxNo = nsd->findIndexByKeyPattern(obj);
            return CatalogHack::getDescriptor(nsd, idxNo);
        }

        void insert(const BSONObj& obj) {
            _client.insert(ns(), obj);
        }

        void remove(const BSONObj& obj) {
            _client.remove(ns(), obj);
        }

        void getLocs(set<DiskLoc>* out) {
            for (boost::shared_ptr<Cursor> c = theDataFileMgr.findAll(ns()); c->ok();
                 c->advance()) {

                out->insert(c->currLoc());
            }
        }

        BSONObj objWithMinKey(int start) {
            BSONObjBuilder startKeyBob;
            startKeyBob.append("", start);
            startKeyBob.appendMinKey("");
            return startKeyBob.obj();
        }

        BSONObj objWithMaxKey(int start) {
            BSONObjBuilder endKeyBob;
            endKeyBob.append("", start);
            endKeyBob.appendMaxKey("");
            return endKeyBob.obj();
        }

        static const char* ns() { return "unittests.QueryStageMergeSort"; }
    private:
        static DBDirectClient _client;
    };

    DBDirectClient QueryStageMergeSortTestBase::_client;

    // SERVER-1205:
    // find($or[{a:1}, {b:1}]).sort({c:1}) with indices {a:1, c:1} and {b:1, c:1}.
    class QueryStageMergeSortPrefixIndex : public QueryStageMergeSortTestBase {
    public:
        void run() {
            Client::WriteContext ctx(ns());

            const int N = 50;

            for (int i = 0; i < N; ++i) {
                insert(BSON("a" << 1 << "c" << i));
                insert(BSON("b" << 1 << "c" << i));
            }

            BSONObj firstIndex = BSON("a" << 1 << "c" << 1);
            BSONObj secondIndex = BSON("b" << 1 << "c" << 1);

            addIndex(firstIndex);
            addIndex(secondIndex);

            WorkingSet* ws = new WorkingSet();
            // Sort by c:1
            MergeSortStageParams msparams;
            msparams.pattern = BSON("c" << 1);
            MergeSortStage* ms = new MergeSortStage(msparams, ws);

            // a:1
            IndexScanParams params;
            params.descriptor = getIndex(firstIndex);
            params.bounds.isSimpleRange = true;
            params.bounds.startKey = objWithMinKey(1);
            params.bounds.endKey = objWithMaxKey(1);
            params.bounds.endKeyInclusive = true;
            params.direction = 1;
            ms->addChild(new IndexScan(params, ws, NULL));

            // b:1
            params.descriptor = getIndex(secondIndex);
            ms->addChild(new IndexScan(params, ws, NULL));

            // Must fetch if we want to easily pull out an obj.
            PlanExecutor runner(ws, new FetchStage(ws, ms, NULL));

            for (int i = 0; i < N; ++i) {
                BSONObj first, second;
                ASSERT_EQUALS(Runner::RUNNER_ADVANCED, runner.getNext(&first, NULL));
                ASSERT_EQUALS(Runner::RUNNER_ADVANCED, runner.getNext(&second, NULL));
                ASSERT_EQUALS(first["c"].numberInt(), second["c"].numberInt());
                ASSERT_EQUALS(i, first["c"].numberInt());
                ASSERT((first.hasField("a") && second.hasField("b"))
                       || (first.hasField("b") && second.hasField("a")));
            }

            // Should be done now.
            BSONObj foo;
            ASSERT_NOT_EQUALS(Runner::RUNNER_ADVANCED, runner.getNext(&foo, NULL));
        }
    };

    // Each inserted document appears in both indices but is deduped and returned properly/sorted.
    class QueryStageMergeSortDups : public QueryStageMergeSortTestBase {
    public:
        void run() {
            Client::WriteContext ctx(ns());

            const int N = 50;

            for (int i = 0; i < N; ++i) {
                insert(BSON("a" << 1 << "b" << 1 << "c" << i));
                insert(BSON("a" << 1 << "b" << 1 << "c" << i));
            }

            BSONObj firstIndex = BSON("a" << 1 << "c" << 1);
            BSONObj secondIndex = BSON("b" << 1 << "c" << 1);

            addIndex(firstIndex);
            addIndex(secondIndex);

            WorkingSet* ws = new WorkingSet();
            // Sort by c:1
            MergeSortStageParams msparams;
            msparams.pattern = BSON("c" << 1);
            MergeSortStage* ms = new MergeSortStage(msparams, ws);

            // a:1
            IndexScanParams params;
            params.descriptor = getIndex(firstIndex);
            params.bounds.isSimpleRange = true;
            params.bounds.startKey = objWithMinKey(1);
            params.bounds.endKey = objWithMaxKey(1);
            params.bounds.endKeyInclusive = true;
            params.direction = 1;
            ms->addChild(new IndexScan(params, ws, NULL));

            // b:1
            params.descriptor = getIndex(secondIndex);
            ms->addChild(new IndexScan(params, ws, NULL));

            PlanExecutor runner(ws, new FetchStage(ws, ms, NULL));

            for (int i = 0; i < N; ++i) {
                BSONObj first, second;
                ASSERT_EQUALS(Runner::RUNNER_ADVANCED, runner.getNext(&first, NULL));
                ASSERT_EQUALS(Runner::RUNNER_ADVANCED, runner.getNext(&second, NULL));
                ASSERT_EQUALS(first["c"].numberInt(), second["c"].numberInt());
                ASSERT_EQUALS(i, first["c"].numberInt());
                ASSERT((first.hasField("a") && second.hasField("b"))
                       || (first.hasField("b") && second.hasField("a")));
            }

            // Should be done now.
            BSONObj foo;
            ASSERT_EQUALS(Runner::RUNNER_EOF, runner.getNext(&foo, NULL));
        }
    };

    // Each inserted document appears in both indices, no deduping, get each result twice.
    class QueryStageMergeSortDupsNoDedup : public QueryStageMergeSortTestBase {
    public:
        void run() {
            Client::WriteContext ctx(ns());

            const int N = 50;

            for (int i = 0; i < N; ++i) {
                insert(BSON("a" << 1 << "b" << 1 << "c" << i));
            }

            BSONObj firstIndex = BSON("a" << 1 << "c" << 1);
            BSONObj secondIndex = BSON("b" << 1 << "c" << 1);

            addIndex(firstIndex);
            addIndex(secondIndex);

            WorkingSet* ws = new WorkingSet();
            // Sort by c:1
            MergeSortStageParams msparams;
            msparams.dedup = false;
            msparams.pattern = BSON("c" << 1);
            MergeSortStage* ms = new MergeSortStage(msparams, ws);

            // a:1
            IndexScanParams params;
            params.descriptor = getIndex(firstIndex);
            params.bounds.isSimpleRange = true;
            params.bounds.startKey = objWithMinKey(1);
            params.bounds.endKey = objWithMaxKey(1);
            params.bounds.endKeyInclusive = true;
            params.direction = 1;
            ms->addChild(new IndexScan(params, ws, NULL));

            // b:1
            params.descriptor = getIndex(secondIndex);
            ms->addChild(new IndexScan(params, ws, NULL));

            PlanExecutor runner(ws, new FetchStage(ws, ms, NULL));

            for (int i = 0; i < N; ++i) {
                BSONObj first, second;
                // We inserted N objects but we get 2 * N from the runner because of dups.
                ASSERT_EQUALS(Runner::RUNNER_ADVANCED, runner.getNext(&first, NULL));
                ASSERT_EQUALS(Runner::RUNNER_ADVANCED, runner.getNext(&second, NULL));
                ASSERT_EQUALS(first["c"].numberInt(), second["c"].numberInt());
                ASSERT_EQUALS(i, first["c"].numberInt());
                ASSERT((first.hasField("a") && second.hasField("b"))
                       || (first.hasField("b") && second.hasField("a")));
            }

            // Should be done now.
            BSONObj foo;
            ASSERT_EQUALS(Runner::RUNNER_EOF, runner.getNext(&foo, NULL));
        }
    };

    // Decreasing indices merged ok.  Basically the test above but decreasing.
    class QueryStageMergeSortPrefixIndexReverse : public QueryStageMergeSortTestBase {
    public:
        void run() {
            Client::WriteContext ctx(ns());

            const int N = 50;

            for (int i = 0; i < N; ++i) {
                // We insert a:1 c:i for i=0..49 but in reverse order for the heck of it.
                insert(BSON("a" << 1 << "c" << N - i - 1));
                insert(BSON("b" << 1 << "c" << i));
            }

            BSONObj firstIndex = BSON("a" << 1 << "c" << -1);
            BSONObj secondIndex = BSON("b" << 1 << "c" << -1);

            addIndex(firstIndex);
            addIndex(secondIndex);

            WorkingSet* ws = new WorkingSet();
            // Sort by c:-1
            MergeSortStageParams msparams;
            msparams.pattern = BSON("c" << -1);
            MergeSortStage* ms = new MergeSortStage(msparams, ws);

            // a:1
            IndexScanParams params;
            params.descriptor = getIndex(firstIndex);
            params.bounds.isSimpleRange = true;
            params.bounds.startKey = objWithMaxKey(1);
            params.bounds.endKey = objWithMinKey(1);
            params.bounds.endKeyInclusive = true;
            // This is the direction along the index.
            params.direction = 1;
            ms->addChild(new IndexScan(params, ws, NULL));

            // b:1
            params.descriptor = getIndex(secondIndex);
            ms->addChild(new IndexScan(params, ws, NULL));

            PlanExecutor runner(ws, new FetchStage(ws, ms, NULL));

            for (int i = 0; i < N; ++i) {
                BSONObj first, second;
                ASSERT_EQUALS(Runner::RUNNER_ADVANCED, runner.getNext(&first, NULL));
                ASSERT_EQUALS(Runner::RUNNER_ADVANCED, runner.getNext(&second, NULL));
                ASSERT_EQUALS(first["c"].numberInt(), second["c"].numberInt());
                ASSERT_EQUALS(N - i - 1, first["c"].numberInt());
                ASSERT((first.hasField("a") && second.hasField("b"))
                       || (first.hasField("b") && second.hasField("a")));
            }

            // Should be done now.
            BSONObj foo;
            ASSERT_EQUALS(Runner::RUNNER_EOF, runner.getNext(&foo, NULL));
        }
    };

    // One stage EOF immediately
    class QueryStageMergeSortOneStageEOF : public QueryStageMergeSortTestBase {
    public:
        void run() {
            Client::WriteContext ctx(ns());

            const int N = 50;

            for (int i = 0; i < N; ++i) {
                insert(BSON("a" << 1 << "c" << i));
                insert(BSON("b" << 1 << "c" << i));
            }

            BSONObj firstIndex = BSON("a" << 1 << "c" << 1);
            BSONObj secondIndex = BSON("b" << 1 << "c" << 1);

            addIndex(firstIndex);
            addIndex(secondIndex);

            WorkingSet* ws = new WorkingSet();
            // Sort by c:1
            MergeSortStageParams msparams;
            msparams.pattern = BSON("c" << 1);
            MergeSortStage* ms = new MergeSortStage(msparams, ws);

            // a:1
            IndexScanParams params;
            params.descriptor = getIndex(firstIndex);
            params.bounds.isSimpleRange = true;
            params.bounds.startKey = objWithMinKey(1);
            params.bounds.endKey = objWithMaxKey(1);
            params.bounds.endKeyInclusive = true;
            params.direction = 1;
            ms->addChild(new IndexScan(params, ws, NULL));

            // b:51 (EOF)
            params.descriptor = getIndex(secondIndex);
            params.bounds.startKey = BSON("" << 51 << "" << MinKey);
            params.bounds.endKey = BSON("" << 51 << "" << MaxKey);
            ms->addChild(new IndexScan(params, ws, NULL));

            PlanExecutor runner(ws, new FetchStage(ws, ms, NULL));

            // Only getting results from the a:1 index scan.
            for (int i = 0; i < N; ++i) {
                BSONObj obj;
                ASSERT_EQUALS(Runner::RUNNER_ADVANCED, runner.getNext(&obj, NULL));
                ASSERT_EQUALS(i, obj["c"].numberInt());
                ASSERT_EQUALS(1, obj["a"].numberInt());
            }

            // Should be done now.
            BSONObj foo;
            ASSERT_EQUALS(Runner::RUNNER_EOF, runner.getNext(&foo, NULL));
        }
    };

    // N stages each have 1 result
    class QueryStageMergeSortManyShort : public QueryStageMergeSortTestBase {
    public:
        void run() {
            Client::WriteContext ctx(ns());

            WorkingSet* ws = new WorkingSet();
            // Sort by foo:1
            MergeSortStageParams msparams;
            msparams.pattern = BSON("foo" << 1);
            MergeSortStage* ms = new MergeSortStage(msparams, ws);

            IndexScanParams params;
            params.bounds.isSimpleRange = true;
            params.bounds.startKey = objWithMinKey(1);
            params.bounds.endKey = objWithMaxKey(1);
            params.bounds.endKeyInclusive = true;
            params.direction = 1;

            int numIndices = 20;
            for (int i = 0; i < numIndices; ++i) {
                // 'a', 'b', ...
                string index(1, 'a' + i);
                insert(BSON(index << 1 << "foo" << i));

                BSONObj indexSpec = BSON(index << 1 << "foo" << 1);
                addIndex(indexSpec);
                params.descriptor = getIndex(indexSpec);
                ms->addChild(new IndexScan(params, ws, NULL));
            }

            PlanExecutor runner(ws, new FetchStage(ws, ms, NULL));

            for (int i = 0; i < numIndices; ++i) {
                BSONObj obj;
                ASSERT_EQUALS(Runner::RUNNER_ADVANCED, runner.getNext(&obj, NULL));
                ASSERT_EQUALS(i, obj["foo"].numberInt());
                string index(1, 'a' + i);
                ASSERT_EQUALS(1, obj[index].numberInt());
            }

            // Should be done now.
            BSONObj foo;
            ASSERT_EQUALS(Runner::RUNNER_EOF, runner.getNext(&foo, NULL));
        }
    };

    // Invalidation mid-run
    class QueryStageMergeSortInvalidation : public QueryStageMergeSortTestBase {
    public:
        void run() {
            Client::WriteContext ctx(ns());

            WorkingSet ws;
            // Sort by foo:1
            MergeSortStageParams msparams;
            msparams.pattern = BSON("foo" << 1);
            auto_ptr<MergeSortStage> ms(new MergeSortStage(msparams, &ws));

            IndexScanParams params;
            params.bounds.isSimpleRange = true;
            params.bounds.startKey = objWithMinKey(1);
            params.bounds.endKey = objWithMaxKey(1);
            params.bounds.endKeyInclusive = true;
            params.direction = 1;

            // Index 'a'+i has foo equal to 'i'.

            int numIndices = 20;
            for (int i = 0; i < numIndices; ++i) {
                // 'a', 'b', ...
                string index(1, 'a' + i);
                insert(BSON(index << 1 << "foo" << i));

                BSONObj indexSpec = BSON(index << 1 << "foo" << 1);
                addIndex(indexSpec);
                params.descriptor = getIndex(indexSpec);
                ms->addChild(new IndexScan(params, &ws, NULL));
            }

            set<DiskLoc> locs;
            getLocs(&locs);

            set<DiskLoc>::iterator it = locs.begin();

            // Get 10 results.  Should be getting results in order of 'locs'.
            int count = 0;
            while (!ms->isEOF() && count < 10) {
                WorkingSetID id;
                PlanStage::StageState status = ms->work(&id);
                if (PlanStage::ADVANCED != status) { continue; }

                WorkingSetMember* member = ws.get(id);
                ASSERT_EQUALS(member->loc, *it);
                BSONElement elt;
                string index(1, 'a' + count);
                ASSERT(member->getFieldDotted(index, &elt));
                ASSERT_EQUALS(1, elt.numberInt());
                ASSERT(member->getFieldDotted("foo", &elt));
                ASSERT_EQUALS(count, elt.numberInt());
                ++count;
                ++it;
            }

            // Invalidate locs[11].  Should force a fetch.  Will still get it back.
            ms->prepareToYield();
            ms->invalidate(*it);
            ms->recoverFromYield();

            // Make sure locs[11] was fetched for us.
            {
                WorkingSetID id;
                PlanStage::StageState status;
                do {
                    status = ms->work(&id);
                } while (PlanStage::ADVANCED != status);

                WorkingSetMember* member = ws.get(id);
                ASSERT(!member->hasLoc());
                ASSERT(member->hasObj());
                string index(1, 'a' + count);
                BSONElement elt;
                ASSERT_TRUE(member->getFieldDotted(index, &elt));
                ASSERT_EQUALS(1, elt.numberInt());
                ASSERT(member->getFieldDotted("foo", &elt));
                ASSERT_EQUALS(count, elt.numberInt());

                ++it;
                ++count;
            }

            // And get the rest.
            while (!ms->isEOF()) {
                WorkingSetID id;
                PlanStage::StageState status = ms->work(&id);
                if (PlanStage::ADVANCED != status) { continue; }

                WorkingSetMember* member = ws.get(id);
                ASSERT_EQUALS(member->loc, *it);
                BSONElement elt;
                string index(1, 'a' + count);
                ASSERT_TRUE(member->getFieldDotted(index, &elt));
                ASSERT_EQUALS(1, elt.numberInt());
                ASSERT(member->getFieldDotted("foo", &elt));
                ASSERT_EQUALS(count, elt.numberInt());
                ++count;
                ++it;
            }
        }
    };

    class All : public Suite {
    public:
        All() : Suite( "query_stage_merge_sort_test" ) { }

        void setupTests() {
            add<QueryStageMergeSortPrefixIndex>();
            add<QueryStageMergeSortDups>();
            add<QueryStageMergeSortDupsNoDedup>();
            add<QueryStageMergeSortPrefixIndexReverse>();
            add<QueryStageMergeSortOneStageEOF>();
            add<QueryStageMergeSortManyShort>();
            add<QueryStageMergeSortInvalidation>();
        }
    }  queryStageMergeSortTest;

}  // namespace

