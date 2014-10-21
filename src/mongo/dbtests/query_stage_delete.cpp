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
 * This file tests db/exec/delete.cpp.
 */

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/delete.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/dbtests/dbtests.h"

namespace QueryStageDelete {

    //
    // Stage-specific tests.
    //

    class QueryStageDeleteBase {
    public:
        QueryStageDeleteBase() : _client(&_txn) {
            Client::WriteContext ctx(&_txn, ns());

            for (size_t i = 0; i < numObj(); ++i) {
                BSONObjBuilder bob;
                bob.append("foo", static_cast<long long int>(i));
                _client.insert(ns(), bob.obj());
            }
        }

        virtual ~QueryStageDeleteBase() {
            Client::WriteContext ctx(&_txn, ns());
            _client.dropCollection(ns());
        }

        void remove(const BSONObj& obj) {
            _client.remove(ns(), obj);
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

        static size_t numObj() { return 50; }

        static const char* ns() { return "unittests.QueryStageDelete"; }

    protected:
        OperationContextImpl _txn;

    private:
        DBDirectClient _client;
    };

    //
    // Test invalidation for the delete stage.  Use the delete stage to delete some objects
    // retrieved by a collscan, then invalidate the upcoming object, then expect the delete stage to
    // skip over it and successfully delete the rest.
    //
    class QueryStageDeleteInvalidateUpcomingObject : public QueryStageDeleteBase {
    public:
        void run() {
            Client::WriteContext ctx(&_txn, ns());

            Collection* coll = ctx.getCollection();

            // Get the DiskLocs that would be returned by an in-order scan.
            vector<DiskLoc> locs;
            getLocs(coll, CollectionScanParams::FORWARD, &locs);

            // Configure the scan.
            CollectionScanParams collScanParams;
            collScanParams.collection = coll;
            collScanParams.direction = CollectionScanParams::FORWARD;
            collScanParams.tailable = false;

            // Configure the delete stage.
            DeleteStageParams deleteStageParams;
            deleteStageParams.isMulti = true;
            deleteStageParams.shouldCallLogOp = false;

            WorkingSet ws;
            DeleteStage deleteStage(&_txn, deleteStageParams, &ws, coll,
                                    new CollectionScan(&_txn, collScanParams, &ws, NULL));

            const DeleteStats* stats =
                static_cast<const DeleteStats*>(deleteStage.getSpecificStats());

            const size_t targetDocIndex = 10;

            while (stats->docsDeleted < targetDocIndex) {
                WorkingSetID id = WorkingSet::INVALID_ID;
                PlanStage::StageState state = deleteStage.work(&id);
                ASSERT_EQUALS(PlanStage::NEED_TIME, state);
            }

            // Remove locs[targetDocIndex];
            deleteStage.saveState();
            deleteStage.invalidate(locs[targetDocIndex], INVALIDATION_DELETION);
            BSONObj targetDoc = coll->docFor(&_txn, locs[targetDocIndex]);
            ASSERT(!targetDoc.isEmpty());
            remove(targetDoc);
            deleteStage.restoreState(&_txn);

            // Remove the rest.
            while (!deleteStage.isEOF()) {
                WorkingSetID id = WorkingSet::INVALID_ID;
                PlanStage::StageState state = deleteStage.work(&id);
                invariant(PlanStage::NEED_TIME == state || PlanStage::IS_EOF == state);
            }

            ASSERT_EQUALS(numObj() - 1, stats->docsDeleted);
        }
    };

    class All : public Suite {
    public:
        All() : Suite("query_stage_delete") {}

        void setupTests() {
            // Stage-specific tests below.
            add<QueryStageDeleteInvalidateUpcomingObject>();
        }
    } all;

}
