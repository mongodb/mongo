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
 * This file tests db/exec/keep_mutations.cpp.
 */

#include <boost/shared_ptr.hpp>

#include "mongo/client/dbclientcursor.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/keep_mutations.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/instance.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/fail_point_registry.h"
#include "mongo/util/fail_point_service.h"

namespace QueryStageKeep {

    class QueryStageKeepBase {
    public:
        QueryStageKeepBase() { }

        virtual ~QueryStageKeepBase() {
            _client.dropCollection(ns());
        }

        void getLocs(set<DiskLoc>* out, Collection* coll) {
            RecordIterator* it = coll->getIterator(DiskLoc(), false,
                                                       CollectionScanParams::FORWARD);
            while (!it->isEOF()) {
                DiskLoc nextLoc = it->getNext();
                out->insert(nextLoc);
            }
            delete it;
        }

        void insert(const BSONObj& obj) {
            _client.insert(ns(), obj);
        }

        void remove(const BSONObj& obj) {
            _client.remove(ns(), obj);
        }

        static const char* ns() { return "unittests.QueryStageKeep"; }

        WorkingSetID getNextResult(PlanStage* stage) {
            while (!stage->isEOF()) {
                WorkingSetID id = WorkingSet::INVALID_ID;
                PlanStage::StageState status = stage->work(&id);
                if (PlanStage::ADVANCED == status) {
                    return id;
                }
            }
            return WorkingSet::INVALID_ID;
        }

    private:
        static DBDirectClient _client;
    };

    DBDirectClient QueryStageKeepBase::_client;

    // Test that we actually merge flagged results.

    //
    // Test that a fetch is passed up when it's not in memory.
    //
    class KeepStageBasic : public QueryStageKeepBase {
    public:
        void run() {
            OperationContextImpl txn;
            Client::WriteContext ctx(&txn, ns());

            Database* db = ctx.ctx().db();
            Collection* coll = db->getCollection(ns());
            if (!coll) {
                coll = db->createCollection(&txn, ns());
            }
            WorkingSet ws;

            // Add 10 objects to the collection.
            for (size_t i = 0; i < 10; ++i) {
                insert(BSON("x" << 1));
            }

            // Create 10 objects that are flagged.
            for (size_t i = 0; i < 10; ++i) {
                WorkingSetID id = ws.allocate();
                WorkingSetMember* member = ws.get(id);
                member->state = WorkingSetMember::OWNED_OBJ;
                member->obj = BSON("x" << 2);
                ws.flagForReview(id);
            }

            // Create a collscan to provide the 10 objects in the collection.
            CollectionScanParams params;
            params.collection = coll;
            params.direction = CollectionScanParams::FORWARD;
            params.tailable = false;
            params.start = DiskLoc();
            CollectionScan* cs = new CollectionScan(params, &ws, NULL);

            // Create a KeepMutations stage to merge in the 10 flagged objects.
            // Takes ownership of 'cs'
            KeepMutationsStage* keep = new KeepMutationsStage(NULL, &ws, cs);

            for (size_t i = 0; i < 10; ++i) {
                WorkingSetID id = getNextResult(keep);
                WorkingSetMember* member = ws.get(id);
                ASSERT_FALSE(ws.isFlagged(id));
                ASSERT_EQUALS(member->obj["x"].numberInt(), 1);
            }

            ASSERT(cs->isEOF());

            // Flagged results *must* be at the end.
            for (size_t i = 0; i < 10; ++i) {
                WorkingSetID id = getNextResult(keep);
                WorkingSetMember* member = ws.get(id);
                ASSERT(ws.isFlagged(id));
                ASSERT_EQUALS(member->obj["x"].numberInt(), 2);
            }
        }
    };

    class All : public Suite {
    public:
        All() : Suite( "query_stage_keep" ) { }

        void setupTests() {
            add<KeepStageBasic>();
        }
    }  queryStageKeepAll;

}  // namespace QueryStageKeep
