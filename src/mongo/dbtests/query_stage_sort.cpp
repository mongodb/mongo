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
#include "mongo/db/exec/mock_stage.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/sort.h"
#include "mongo/db/index/catalog_hack.h"
#include "mongo/db/instance.h"
#include "mongo/db/json.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/dbtests/dbtests.h"

/**
 * This file tests db/exec/sort.cpp
 */

namespace QueryStageSortTests {

    class QueryStageSortTestBase {
    public:
        QueryStageSortTestBase() { }

        void fillData() {
            for (int i = 0; i < numObj(); ++i) {
                insert(BSON("foo" << i));
            }
        }

        virtual ~QueryStageSortTestBase() {
            _client.dropCollection(ns());
        }

        void insert(const BSONObj& obj) {
            _client.insert(ns(), obj);
        }

        void getLocs(set<DiskLoc>* locs) {
            for (boost::shared_ptr<Cursor> c = theDataFileMgr.findAll(ns());
                 c->ok(); c->advance()) {

                locs->insert(c->currLoc());
            }
        }

        /**
         * We feed a mix of (key, unowned, owned) data to the sort stage.
         */
        void insertVarietyOfObjects(MockStage* ms) {
            set<DiskLoc> locs;
            getLocs(&locs);

            set<DiskLoc>::iterator it = locs.begin();

            for (int i = 0; i < numObj(); ++i, ++it) {
                int which = i % 3;

                if (0 == which) {
                // Insert some unowned obj data.
                    WorkingSetMember member;
                    member.state = WorkingSetMember::LOC_AND_UNOWNED_OBJ;
                    member.loc = *it;
                    member.obj = member.loc.obj();
                    ASSERT(!member.obj.isOwned());
                    ms->pushBack(member);
                }
                else if (1 == which) {
                // Insert some key data.
                    WorkingSetMember member;
                    member.state = WorkingSetMember::LOC_AND_IDX;
                    member.loc = *it;
                    member.keyData.push_back(IndexKeyDatum(BSON("foo" << 1), BSON("" << i)));
                    ms->pushBack(member);
                }
                else {
                // Insert some owned obj data.
                    WorkingSetMember member;
                    member.state = WorkingSetMember::OWNED_OBJ;
                    member.obj = it->obj().getOwned();
                    ASSERT(member.obj.isOwned());
                    ms->pushBack(member);
                }
            }
        }

        // Return a value in the set {-1, 0, 1} to represent the sign of parameter i.  Used to
        // normalize woCompare calls.
        int sgn(int i) {
            if (i == 0)
                return 0;
            return i > 0 ? 1 : -1;
        }

        /**
         * A template used by many tests below.
         * Fill out numObj objects, sort them in the order provided by 'direction'.
         * If extAllowed is true, sorting will use use external sorting if available.
         * If limit is not zero, we limit the output of the sort stage to 'limit' results.
         */
        void sortAndCheck(int direction) {
            WorkingSet* ws = new WorkingSet();
            MockStage* ms = new MockStage(ws);

            // Insert a mix of the various types of data.
            insertVarietyOfObjects(ms);

            SortStageParams params;
            params.pattern = BSON("foo" << direction);

            // Must fetch so we can look at the doc as a BSONObj.
            PlanExecutor runner(ws, new FetchStage(ws, new SortStage(params, ws, ms), NULL));

            // Look at pairs of objects to make sure that the sort order is pairwise (and therefore
            // totally) correct.
            BSONObj last;
            ASSERT_EQUALS(Runner::RUNNER_ADVANCED, runner.getNext(&last, NULL));

            // Count 'last'.
            int count = 1;

            BSONObj current;
            while (Runner::RUNNER_ADVANCED == runner.getNext(&current, NULL)) {
                int cmp = sgn(current.woSortOrder(last, params.pattern));
                // The next object should be equal to the previous or oriented according to the sort
                // pattern.
                ASSERT(cmp == 0 || cmp == 1);
                ++count;
                last = current;
            }

            // No limit, should get all objects back.
            ASSERT_EQUALS(numObj(), count);
        }

        virtual int numObj() = 0;

        static const char* ns() { return "unittests.QueryStageSort"; }
    private:
        static DBDirectClient _client;
    };

    DBDirectClient QueryStageSortTestBase::_client;

    // Sort some small # of results in increasing order.
    class QueryStageSortInc: public QueryStageSortTestBase {
    public:
        virtual int numObj() { return 100; }

        void run() {
            Client::WriteContext ctx(ns());
            fillData();
            sortAndCheck(1);
        }
    };

    // Sort some small # of results in decreasing order.
    class QueryStageSortDec : public QueryStageSortTestBase {
    public:
        virtual int numObj() { return 100; }

        void run() {
            Client::WriteContext ctx(ns());
            fillData();
            sortAndCheck(-1);
        }
    };

    // Sort a big bunch of objects.
    class QueryStageSortExt : public QueryStageSortTestBase {
    public:
        virtual int numObj() { return 10000; }

        void run() {
            Client::WriteContext ctx(ns());
            fillData();
            sortAndCheck(-1);
        }
    };

    // Invalidation of everything fed to sort.
    class QueryStageSortInvalidation : public QueryStageSortTestBase {
    public:
        virtual int numObj() { return 2000; }

        void run() {
            Client::WriteContext ctx(ns());
            fillData();

            // The data we're going to later invalidate.
            set<DiskLoc> locs;
            getLocs(&locs);

            // Build the mock stage which feeds the data.
            WorkingSet ws;
            auto_ptr<MockStage> ms(new MockStage(&ws));
            insertVarietyOfObjects(ms.get());

            SortStageParams params;
            params.pattern = BSON("foo" << 1);
            auto_ptr<SortStage> ss(new SortStage(params, &ws, ms.get()));

            const int firstRead = 10;

            // Have sort read in data from the mock stage.
            for (int i = 0; i < firstRead; ++i) {
                WorkingSetID id;
                PlanStage::StageState status = ss->work(&id);
                ASSERT_NOT_EQUALS(PlanStage::ADVANCED, status);
            }

            // We should have read in the first 'firstRead' locs.  Invalidate the first.
            ss->prepareToYield();
            set<DiskLoc>::iterator it = locs.begin();
            ss->invalidate(*it++);
            ss->recoverFromYield();

            // Read the rest of the data from the mock stage.
            while (!ms->isEOF()) {
                WorkingSetID id;
                ss->work(&id);
            }

            // Release to prevent double-deletion.
            ms.release();

            // Let's just invalidate everything now.
            ss->prepareToYield();
            while (it != locs.end()) {
                ss->invalidate(*it++);
            }
            ss->recoverFromYield();

            // The sort should still work.
            int count = 0;
            while (!ss->isEOF()) {
                WorkingSetID id;
                PlanStage::StageState status = ss->work(&id);
                if (PlanStage::ADVANCED != status) { continue; }
                WorkingSetMember* member = ws.get(id);
                ASSERT(member->hasObj());
                ASSERT(!member->hasLoc());
                ++count;
            }

            ASSERT_EQUALS(count, numObj());
        }
    };

    class All : public Suite {
    public:
        All() : Suite( "query_stage_sort_test" ) { }

        void setupTests() {
            add<QueryStageSortInc>();
            add<QueryStageSortDec>();
            add<QueryStageSortExt>();
            add<QueryStageSortInvalidation>();
        }
    }  queryStageSortTest;

}  // namespace

