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

#include "mongo/client/dbclientcursor.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/fetch.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/queued_data_stage.h"
#include "mongo/db/exec/sort.h"
#include "mongo/db/json.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/dbtests/dbtests.h"

/**
 * This file tests db/exec/sort.cpp
 */

namespace QueryStageSortTests {

    class QueryStageSortTestBase {
    public:
        QueryStageSortTestBase() : _client(&_txn) {
        
        }

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

        void getLocs(set<RecordId>* out, Collection* coll) {
            RecordIterator* it = coll->getIterator(&_txn);
            while (!it->isEOF()) {
                RecordId nextLoc = it->getNext();
                out->insert(nextLoc);
            }
            delete it;
        }

        /**
         * We feed a mix of (key, unowned, owned) data to the sort stage.
         */
        void insertVarietyOfObjects(QueuedDataStage* ms, Collection* coll) {
            set<RecordId> locs;
            getLocs(&locs, coll);

            set<RecordId>::iterator it = locs.begin();

            for (int i = 0; i < numObj(); ++i, ++it) {
                ASSERT_FALSE(it == locs.end());

                // Insert some owned obj data.
                WorkingSetMember member;
                member.loc = *it;
                member.state = WorkingSetMember::LOC_AND_UNOWNED_OBJ;
                member.obj = coll->docFor(&_txn, *it);
                ms->pushBack(member);
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
        void sortAndCheck(int direction, Collection* coll) {
            WorkingSet* ws = new WorkingSet();
            QueuedDataStage* ms = new QueuedDataStage(ws);

            // Insert a mix of the various types of data.
            insertVarietyOfObjects(ms, coll);

            SortStageParams params;
            params.collection = coll;
            params.pattern = BSON("foo" << direction);
            params.limit = limit();

            // Must fetch so we can look at the doc as a BSONObj.
            PlanExecutor* rawExec;
            Status status =
                PlanExecutor::make(&_txn,
                                   ws,
                                   new FetchStage(&_txn, ws,
                                                  new SortStage(params, ws, ms), NULL, coll),
                                   coll, PlanExecutor::YIELD_MANUAL, &rawExec);
            ASSERT_OK(status);
            boost::scoped_ptr<PlanExecutor> exec(rawExec);

            // Look at pairs of objects to make sure that the sort order is pairwise (and therefore
            // totally) correct.
            BSONObj last;
            ASSERT_EQUALS(PlanExecutor::ADVANCED, exec->getNext(&last, NULL));

            // Count 'last'.
            int count = 1;

            BSONObj current;
            while (PlanExecutor::ADVANCED == exec->getNext(&current, NULL)) {
                int cmp = sgn(current.woSortOrder(last, params.pattern));
                // The next object should be equal to the previous or oriented according to the sort
                // pattern.
                ASSERT(cmp == 0 || cmp == 1);
                ++count;
                last = current;
            }

            checkCount(count);
        }

        /**
         * Check number of results returned from sort.
         */
        void checkCount(int count) {
            // No limit, should get all objects back.
            // Otherwise, result set should be smaller of limit and input data size.
            if (limit() > 0 && limit() < numObj()) {
                ASSERT_EQUALS(limit(), count);
            }
            else {
                ASSERT_EQUALS(numObj(), count);
            }
        }

        virtual int numObj() = 0;

        // Returns sort limit
        // Leave as 0 to disable limit.
        virtual int limit() const { return 0; };


        static const char* ns() { return "unittests.QueryStageSort"; }

    protected:
        OperationContextImpl _txn;
        DBDirectClient _client;
    };


    // Sort some small # of results in increasing order.
    class QueryStageSortInc: public QueryStageSortTestBase {
    public:
        virtual int numObj() { return 100; }

        void run() {
            Client::WriteContext ctx(&_txn, ns());
            Database* db = ctx.ctx().db();
            Collection* coll = db->getCollection(ns());
            if (!coll) {
                WriteUnitOfWork wuow(&_txn);
                coll = db->createCollection(&_txn, ns());
                wuow.commit();
            }

            fillData();
            sortAndCheck(1, coll);
        }
    };

    // Sort some small # of results in decreasing order.
    class QueryStageSortDec : public QueryStageSortTestBase {
    public:
        virtual int numObj() { return 100; }

        void run() {
            Client::WriteContext ctx(&_txn, ns());
            Database* db = ctx.ctx().db();
            Collection* coll = db->getCollection(ns());
            if (!coll) {
                WriteUnitOfWork wuow(&_txn);
                coll = db->createCollection(&_txn, ns());
                wuow.commit();
            }

            fillData();
            sortAndCheck(-1, coll);
        }
    };

    // Sort in descreasing order with limit applied
    template <int LIMIT>
    class QueryStageSortDecWithLimit : public QueryStageSortDec {
    public:
        virtual int limit() const {
            return LIMIT;
        }
    };

    // Sort a big bunch of objects.
    class QueryStageSortExt : public QueryStageSortTestBase {
    public:
        virtual int numObj() { return 10000; }

        void run() {
            Client::WriteContext ctx(&_txn, ns());
            Database* db = ctx.ctx().db();
            Collection* coll = db->getCollection(ns());
            if (!coll) {
                WriteUnitOfWork wuow(&_txn);
                coll = db->createCollection(&_txn, ns());
                wuow.commit();
            }

            fillData();
            sortAndCheck(-1, coll);
        }
    };

    // Invalidation of everything fed to sort.
    class QueryStageSortInvalidation : public QueryStageSortTestBase {
    public:
        virtual int numObj() { return 2000; }

        void run() {
            Client::WriteContext ctx(&_txn, ns());
            Database* db = ctx.ctx().db();
            Collection* coll = db->getCollection(ns());
            if (!coll) {
                WriteUnitOfWork wuow(&_txn);
                coll = db->createCollection(&_txn, ns());
                wuow.commit();
            }

            fillData();

            // The data we're going to later invalidate.
            set<RecordId> locs;
            getLocs(&locs, coll);

            // Build the mock scan stage which feeds the data.
            WorkingSet ws;
            auto_ptr<QueuedDataStage> ms(new QueuedDataStage(&ws));
            insertVarietyOfObjects(ms.get(), coll);

            SortStageParams params;
            params.collection = coll;
            params.pattern = BSON("foo" << 1);
            params.limit = limit();
            auto_ptr<SortStage> ss(new SortStage(params, &ws, ms.get()));

            const int firstRead = 10;

            // Have sort read in data from the mock stage.
            for (int i = 0; i < firstRead; ++i) {
                WorkingSetID id = WorkingSet::INVALID_ID;
                PlanStage::StageState status = ss->work(&id);
                ASSERT_NOT_EQUALS(PlanStage::ADVANCED, status);
            }

            // We should have read in the first 'firstRead' locs.  Invalidate the first.
            ss->saveState();
            set<RecordId>::iterator it = locs.begin();
            ss->invalidate(&_txn, *it++, INVALIDATION_DELETION);
            ss->restoreState(&_txn);

            // Read the rest of the data from the mock stage.
            while (!ms->isEOF()) {
                WorkingSetID id = WorkingSet::INVALID_ID;
                ss->work(&id);
            }

            // Release to prevent double-deletion.
            ms.release();

            // Let's just invalidate everything now.
            ss->saveState();
            while (it != locs.end()) {
                ss->invalidate(&_txn, *it++, INVALIDATION_DELETION);
            }
            ss->restoreState(&_txn);

            // Invalidation of data in the sort stage fetches it but passes it through.
            int count = 0;
            while (!ss->isEOF()) {
                WorkingSetID id = WorkingSet::INVALID_ID;
                PlanStage::StageState status = ss->work(&id);
                if (PlanStage::ADVANCED != status) { continue; }
                WorkingSetMember* member = ws.get(id);
                ASSERT(member->hasObj());
                ASSERT(!member->hasLoc());
                ++count;
            }

            // Returns all docs.
            ASSERT_EQUALS(limit() ? limit() : numObj(), count);
        }
    };

    // Invalidation of everything fed to sort with limit enabled.
    // Limit size of working set within sort stage to a small number
    // Sort stage implementation should not try to invalidate DiskLocc that
    // are no longer in the working set.
    template<int LIMIT>
    class QueryStageSortInvalidationWithLimit : public QueryStageSortInvalidation {
    public:
        virtual int limit() const {
            return LIMIT;
        }
    };

    // Should error out if we sort with parallel arrays.
    class QueryStageSortParallelArrays : public QueryStageSortTestBase {
    public:
        virtual int numObj() { return 100; }

        void run() {
            Client::WriteContext ctx(&_txn, ns());
            Database* db = ctx.ctx().db();
            Collection* coll = db->getCollection(ns());
            if (!coll) {
                WriteUnitOfWork wuow(&_txn);
                coll = db->createCollection(&_txn, ns());
                wuow.commit();
            }

            WorkingSet* ws = new WorkingSet();
            QueuedDataStage* ms = new QueuedDataStage(ws);

            for (int i = 0; i < numObj(); ++i) {
                WorkingSetMember member;
                member.state = WorkingSetMember::OWNED_OBJ;

                member.obj = fromjson("{a: [1,2,3], b:[1,2,3], c:[1,2,3], d:[1,2,3,4]}");
                ms->pushBack(member);

                member.obj = fromjson("{a:1, b:1, c:1}");
                ms->pushBack(member);
            }

            SortStageParams params;
            params.collection = coll;
            params.pattern = BSON("b" << -1 << "c" << 1 << "a" << 1);
            params.limit = 0;

            // We don't get results back since we're sorting some parallel arrays.
            PlanExecutor* rawExec;
            Status status =
                PlanExecutor::make(&_txn,
                                   ws,
                                   new FetchStage(&_txn,
                                                  ws,
                                                  new SortStage(params, ws, ms), NULL, coll),
                                   coll, PlanExecutor::YIELD_MANUAL, &rawExec);
            boost::scoped_ptr<PlanExecutor> exec(rawExec);

            PlanExecutor::ExecState runnerState = exec->getNext(NULL, NULL);
            ASSERT_EQUALS(PlanExecutor::FAILURE, runnerState);
        }
    };

    class All : public Suite {
    public:
        All() : Suite( "query_stage_sort_test" ) { }

        void setupTests() {
            add<QueryStageSortInc>();
            add<QueryStageSortDec>();
            // Sort with limit has a general limiting strategy for limit > 1
            add<QueryStageSortDecWithLimit<10> >();
            // and a special case for limit == 1
            add<QueryStageSortDecWithLimit<1> >();
            add<QueryStageSortExt>();
            add<QueryStageSortInvalidation>();
            add<QueryStageSortInvalidationWithLimit<10> >();
            add<QueryStageSortInvalidationWithLimit<1> >();
            add<QueryStageSortParallelArrays>();
        }
    };

    SuiteInstance<All> queryStageSortTest;

}  // namespace

