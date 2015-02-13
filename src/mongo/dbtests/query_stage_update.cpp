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

/**
 * This file tests db/exec/update.cpp (UpdateStage).
 */

#include <boost/scoped_ptr.hpp>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/eof.h"
#include "mongo/db/exec/update.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/ops/update_driver.h"
#include "mongo/db/ops/update_lifecycle_impl.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/dbtests/dbtests.h"

namespace QueryStageUpdate {

    using boost::scoped_ptr;
    using std::auto_ptr;
    using std::vector;

    class QueryStageUpdateBase {
    public:
        QueryStageUpdateBase()
            : _client(&_txn),
              _ns("unittests.QueryStageUpdate"),
              _nsString(StringData(ns())) {
            Client::WriteContext ctx(&_txn, ns());
            _client.dropCollection(ns());
            _client.createCollection(ns());
        }

        virtual ~QueryStageUpdateBase() {
            Client::WriteContext ctx(&_txn, ns());
            _client.dropCollection(ns());
        }

        void insert(const BSONObj& doc) {
            _client.insert(ns(), doc);
        }

        void remove(const BSONObj& obj) {
            _client.remove(ns(), obj);
        }

        size_t count(const BSONObj& query) {
            return _client.count(ns(), query, 0, 0, 0);
        }

        CanonicalQuery* canonicalize(const BSONObj& query) {
            CanonicalQuery* cq;
            Status status = CanonicalQuery::canonicalize(ns(), query, &cq);
            ASSERT_OK(status);
            return cq;
        }

        /**
         * Runs the update operation by calling work until EOF. Asserts that
         * the update stage always returns NEED_TIME.
         */
        void runUpdate(UpdateStage* updateStage) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState state = PlanStage::NEED_TIME;
            while (PlanStage::IS_EOF != state) {
                ASSERT_EQUALS(PlanStage::NEED_TIME, state);
                state = updateStage->work(&id);
            }
        }

        /**
         * Returns a vector of all of the documents currently in 'collection'.
         *
         * Uses a forward collection scan stage to get the docs, and populates 'out' with
         * the results.
         */
        void getCollContents(Collection* collection, vector<BSONObj>* out) {
            WorkingSet ws;

            CollectionScanParams params;
            params.collection = collection;
            params.direction = CollectionScanParams::FORWARD;
            params.tailable = false;

            scoped_ptr<CollectionScan> scan(new CollectionScan(&_txn, params, &ws, NULL));
            while (!scan->isEOF()) {
                WorkingSetID id = WorkingSet::INVALID_ID;
                PlanStage::StageState state = scan->work(&id);
                if (PlanStage::ADVANCED == state) {
                    WorkingSetMember* member = ws.get(id);
                    verify(member->hasObj());
                    out->push_back(member->obj.value());
                }
            }
        }

        void getLocs(Collection* collection,
                     CollectionScanParams::Direction direction,
                     vector<RecordId>* out) {
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

        /**
         * Asserts that 'objs' contains 'expectedDoc'.
         */
        void assertHasDoc(const vector<BSONObj>& objs, const BSONObj& expectedDoc) {
            bool foundDoc = false;
            for (size_t i = 0; i < objs.size(); i++) {
                if (0 == objs[i].woCompare(expectedDoc)) {
                    foundDoc = true;
                    break;
                }
            }
            ASSERT(foundDoc);
        }

        const char* ns() { return _ns.c_str(); }

        const NamespaceString& nsString() { return _nsString; }

    protected:
        OperationContextImpl _txn;

    private:
        DBDirectClient _client;

        std::string _ns;
        NamespaceString _nsString;
    };

    /**
     * Test an upsert into an empty collection.
     */
    class QueryStageUpdateUpsertEmptyColl : public QueryStageUpdateBase {
    public:
        void run() {
            // Run the update.
            {
                Client::WriteContext ctx(&_txn, ns());
                Client& c = cc();
                CurOp& curOp = *c.curop();
                OpDebug* opDebug = &curOp.debug();
                UpdateDriver driver( (UpdateDriver::Options()) );
                Collection* collection = ctx.getCollection();

                // Collection should be empty.
                ASSERT_EQUALS(0U, count(BSONObj()));

                UpdateRequest request(nsString());
                UpdateLifecycleImpl updateLifecycle(false, nsString());
                request.setLifecycle(&updateLifecycle);

                // Update is the upsert {_id: 0, x: 1}, {$set: {y: 2}}.
                BSONObj query = fromjson("{_id: 0, x: 1}");
                BSONObj updates = fromjson("{$set: {y: 2}}");

                request.setUpsert();
                request.setQuery(query);
                request.setUpdates(updates);

                ASSERT_OK(driver.parse(request.getUpdates(), request.isMulti()));

                // Setup update params.
                UpdateStageParams params(&request, &driver, opDebug);
                scoped_ptr<CanonicalQuery> cq(canonicalize(query));
                params.canonicalQuery = cq.get();

                scoped_ptr<WorkingSet> ws(new WorkingSet());
                auto_ptr<EOFStage> eofStage(new EOFStage());

                scoped_ptr<UpdateStage> updateStage(
                    new UpdateStage(&_txn, params, ws.get(), collection, eofStage.release()));

                runUpdate(updateStage.get());
            }

            // Verify the contents of the resulting collection.
            {
                AutoGetCollectionForRead ctx(&_txn, ns());
                Collection* collection = ctx.getCollection();

                vector<BSONObj> objs;
                getCollContents(collection, &objs);

                // Expect a single document, {_id: 0, x: 1, y: 2}.
                ASSERT_EQUALS(1U, objs.size());
                ASSERT_EQUALS(objs[0], fromjson("{_id: 0, x: 1, y: 2}"));
            }
        }
    };

    /**
     * Test receipt of an invalidation: case in which the document about to updated
     * is deleted.
     */
    class QueryStageUpdateSkipInvalidatedDoc : public QueryStageUpdateBase {
    public:
        void run() {
            // Run the update.
            {
                Client::WriteContext ctx(&_txn, ns());

                // Populate the collection.
                for (int i = 0; i < 10; ++i) {
                    insert(BSON("_id" << i << "foo" << i));
                }
                ASSERT_EQUALS(10U, count(BSONObj()));

                Client& c = cc();
                CurOp& curOp = *c.curop();
                OpDebug* opDebug = &curOp.debug();
                UpdateDriver driver( (UpdateDriver::Options()) );
                Database* db = ctx.db();
                Collection* coll = db->getCollection(ns());

                // Get the RecordIds that would be returned by an in-order scan.
                vector<RecordId> locs;
                getLocs(coll, CollectionScanParams::FORWARD, &locs);

                UpdateRequest request(nsString());
                UpdateLifecycleImpl updateLifecycle(false, nsString());
                request.setLifecycle(&updateLifecycle);

                // Update is a multi-update that sets 'bar' to 3 in every document
                // where foo is less than 5.
                BSONObj query = fromjson("{foo: {$lt: 5}}");
                BSONObj updates = fromjson("{$set: {bar: 3}}");

                request.setMulti();
                request.setQuery(query);
                request.setUpdates(updates);

                ASSERT_OK(driver.parse(request.getUpdates(), request.isMulti()));

                // Configure the scan.
                CollectionScanParams collScanParams;
                collScanParams.collection = coll;
                collScanParams.direction = CollectionScanParams::FORWARD;
                collScanParams.tailable = false;

                // Configure the update.
                UpdateStageParams updateParams(&request, &driver, opDebug);
                scoped_ptr<CanonicalQuery> cq(canonicalize(query));
                updateParams.canonicalQuery = cq.get();

                scoped_ptr<WorkingSet> ws(new WorkingSet());
                auto_ptr<CollectionScan> cs(
                    new CollectionScan(&_txn, collScanParams, ws.get(), cq->root()));

                scoped_ptr<UpdateStage> updateStage(
                    new UpdateStage(&_txn, updateParams, ws.get(), coll, cs.release()));

                const UpdateStats* stats =
                    static_cast<const UpdateStats*>(updateStage->getSpecificStats());

                const size_t targetDocIndex = 3;

                while (stats->nModified < targetDocIndex) {
                    WorkingSetID id = WorkingSet::INVALID_ID;
                    PlanStage::StageState state = updateStage->work(&id);
                    ASSERT_EQUALS(PlanStage::NEED_TIME, state);
                }

                // Remove locs[targetDocIndex];
                updateStage->saveState();
                updateStage->invalidate(&_txn, locs[targetDocIndex], INVALIDATION_DELETION);
                BSONObj targetDoc = coll->docFor(&_txn, locs[targetDocIndex]).value();
                ASSERT(!targetDoc.isEmpty());
                remove(targetDoc);
                updateStage->restoreState(&_txn);

                // Do the remaining updates.
                while (!updateStage->isEOF()) {
                    WorkingSetID id = WorkingSet::INVALID_ID;
                    PlanStage::StageState state = updateStage->work(&id);
                    ASSERT(PlanStage::NEED_TIME == state || PlanStage::IS_EOF == state);
                }

                // 4 of the 5 matching documents should have been modified (one was deleted).
                ASSERT_EQUALS(4U, stats->nModified);
                ASSERT_EQUALS(4U, stats->nMatched);
            }

            // Check the contents of the collection.
            {
                AutoGetCollectionForRead ctx(&_txn, ns());
                Collection* collection = ctx.getCollection();

                vector<BSONObj> objs;
                getCollContents(collection, &objs);

                // Verify that the collection now has 9 docs (one was deleted).
                ASSERT_EQUALS(9U, objs.size());

                // Make sure that the collection has certain documents.
                assertHasDoc(objs, fromjson("{_id: 0, foo: 0, bar: 3}"));
                assertHasDoc(objs, fromjson("{_id: 1, foo: 1, bar: 3}"));
                assertHasDoc(objs, fromjson("{_id: 2, foo: 2, bar: 3}"));
                assertHasDoc(objs, fromjson("{_id: 4, foo: 4, bar: 3}"));
                assertHasDoc(objs, fromjson("{_id: 5, foo: 5}"));
                assertHasDoc(objs, fromjson("{_id: 6, foo: 6}"));
            }
        }
    };

    class All : public Suite {
    public:
        All() : Suite("query_stage_update") {}

        void setupTests() {
            // Stage-specific tests below.
            add<QueryStageUpdateUpsertEmptyColl>();
            add<QueryStageUpdateSkipInvalidatedDoc>();
        }
    };

    SuiteInstance<All> all;

} // namespace QueryStageUpdate
