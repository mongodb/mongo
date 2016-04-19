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

#include "mongo/platform/basic.h"

#include "mongo/client/dbclientcursor.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/eof.h"
#include "mongo/db/exec/keep_mutations.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/fail_point_registry.h"
#include "mongo/util/fail_point_service.h"

namespace QueryStageKeep {

using std::set;
using std::shared_ptr;
using std::unique_ptr;
using stdx::make_unique;

class QueryStageKeepBase {
public:
    QueryStageKeepBase() : _client(&_txn) {}

    virtual ~QueryStageKeepBase() {
        _client.dropCollection(ns());
    }

    void getLocs(set<RecordId>* out, Collection* coll) {
        auto cursor = coll->getCursor(&_txn);
        while (auto record = cursor->next()) {
            out->insert(record->id);
        }
    }

    void insert(const BSONObj& obj) {
        _client.insert(ns(), obj);
    }

    void remove(const BSONObj& obj) {
        _client.remove(ns(), obj);
    }

    static const char* ns() {
        return "unittests.QueryStageKeep";
    }

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

protected:
    const ServiceContext::UniqueOperationContext _txnPtr = cc().makeOperationContext();
    OperationContext& _txn = *_txnPtr;
    DBDirectClient _client;
};


// Test that we actually merge flagged results.

//
// Test that a fetch is passed up when it's not in memory.
//
class KeepStageBasic : public QueryStageKeepBase {
public:
    void run() {
        OldClientWriteContext ctx(&_txn, ns());
        Database* db = ctx.db();
        Collection* coll = db->getCollection(ns());
        if (!coll) {
            WriteUnitOfWork wuow(&_txn);
            coll = db->createCollection(&_txn, ns());
            wuow.commit();
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
            member->obj = Snapshotted<BSONObj>(SnapshotId(), BSON("x" << 2));
            member->transitionToOwnedObj();
            ws.flagForReview(id);
        }

        // Create a collscan to provide the 10 objects in the collection.
        CollectionScanParams params;
        params.collection = coll;
        params.direction = CollectionScanParams::FORWARD;
        params.tailable = false;
        params.start = RecordId();
        CollectionScan* cs = new CollectionScan(&_txn, params, &ws, NULL);

        // Create a KeepMutations stage to merge in the 10 flagged objects.
        // Takes ownership of 'cs'
        MatchExpression* nullFilter = NULL;
        auto keep = make_unique<KeepMutationsStage>(&_txn, nullFilter, &ws, cs);

        for (size_t i = 0; i < 10; ++i) {
            WorkingSetID id = getNextResult(keep.get());
            WorkingSetMember* member = ws.get(id);
            ASSERT_FALSE(ws.isFlagged(id));
            ASSERT_EQUALS(member->obj.value()["x"].numberInt(), 1);
        }

        {
            WorkingSetID out;
            ASSERT_EQ(cs->work(&out), PlanStage::IS_EOF);
        }

        // Flagged results *must* be at the end.
        for (size_t i = 0; i < 10; ++i) {
            WorkingSetID id = getNextResult(keep.get());
            WorkingSetMember* member = ws.get(id);
            ASSERT(ws.isFlagged(id));
            ASSERT_EQUALS(member->obj.value()["x"].numberInt(), 2);
        }
    }
};

/**
 * SERVER-15580: test that the KeepMutationsStage behaves correctly if additional results are
 * flagged after some flagged results have already been returned.
 */
class KeepStageFlagAdditionalAfterStreamingStarts : public QueryStageKeepBase {
public:
    void run() {
        OldClientWriteContext ctx(&_txn, ns());

        Database* db = ctx.db();
        Collection* coll = db->getCollection(ns());
        if (!coll) {
            WriteUnitOfWork wuow(&_txn);
            coll = db->createCollection(&_txn, ns());
            wuow.commit();
        }
        WorkingSet ws;

        std::set<WorkingSetID> expectedResultIds;
        std::set<WorkingSetID> resultIds;

        // Create a KeepMutationsStage with an EOF child, and flag 50 objects.  We expect these
        // objects to be returned by the KeepMutationsStage.
        MatchExpression* nullFilter = NULL;
        auto keep = make_unique<KeepMutationsStage>(&_txn, nullFilter, &ws, new EOFStage(&_txn));
        for (size_t i = 0; i < 50; ++i) {
            WorkingSetID id = ws.allocate();
            WorkingSetMember* member = ws.get(id);
            member->obj = Snapshotted<BSONObj>(SnapshotId(), BSON("x" << 1));
            member->transitionToOwnedObj();
            ws.flagForReview(id);
            expectedResultIds.insert(id);
        }

        // Call work() on the KeepMutationsStage.  The stage should start streaming the
        // already-flagged objects.
        WorkingSetID id = getNextResult(keep.get());
        resultIds.insert(id);

        // Flag more objects, then call work() again on the KeepMutationsStage, and expect none
        // of the newly-flagged objects to be returned (the KeepMutationsStage does not
        // incorporate objects flagged since the streaming phase started).
        //
        // This condition triggers SERVER-15580 (the new flagging causes a rehash of the
        // unordered_set "WorkingSet::_flagged", which invalidates all iterators, which were
        // previously being dereferenced in KeepMutationsStage::work()).
        // Note that std::unordered_set<>::insert() triggers a rehash if the new number of
        // elements is greater than or equal to max_load_factor()*bucket_count().
        size_t rehashSize =
            static_cast<size_t>(ws.getFlagged().max_load_factor() * ws.getFlagged().bucket_count());
        while (ws.getFlagged().size() <= rehashSize) {
            WorkingSetID id = ws.allocate();
            WorkingSetMember* member = ws.get(id);
            member->obj = Snapshotted<BSONObj>(SnapshotId(), BSON("x" << 1));
            member->transitionToOwnedObj();
            ws.flagForReview(id);
        }
        while ((id = getNextResult(keep.get())) != WorkingSet::INVALID_ID) {
            resultIds.insert(id);
        }

        // Assert that only the first 50 objects were returned.
        ASSERT(expectedResultIds == resultIds);
    }
};

class All : public Suite {
public:
    All() : Suite("query_stage_keep") {}

    void setupTests() {
        add<KeepStageBasic>();
        add<KeepStageFlagAdditionalAfterStreamingStarts>();
    }
};

SuiteInstance<All> queryStageKeepAll;

}  // namespace QueryStageKeep
