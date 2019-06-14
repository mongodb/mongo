/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

/**
 * This file tests db/exec/delete.cpp.
 */

#include "mongo/platform/basic.h"

#include <memory>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/delete.h"
#include "mongo/db/exec/queued_data_stage.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/service_context.h"
#include "mongo/dbtests/dbtests.h"

namespace QueryStageDelete {

using std::unique_ptr;
using std::vector;

static const NamespaceString nss("unittests.QueryStageDelete");

//
// Stage-specific tests.
//

class QueryStageDeleteBase {
public:
    QueryStageDeleteBase() : _client(&_opCtx) {
        dbtests::WriteContextForTests ctx(&_opCtx, nss.ns());

        for (size_t i = 0; i < numObj(); ++i) {
            BSONObjBuilder bob;
            bob.append("_id", static_cast<long long int>(i));
            bob.append("foo", static_cast<long long int>(i));
            _client.insert(nss.ns(), bob.obj());
        }
    }

    virtual ~QueryStageDeleteBase() {
        dbtests::WriteContextForTests ctx(&_opCtx, nss.ns());
        _client.dropCollection(nss.ns());
    }

    void remove(const BSONObj& obj) {
        _client.remove(nss.ns(), obj);
    }

    void getRecordIds(Collection* collection,
                      CollectionScanParams::Direction direction,
                      vector<RecordId>* out) {
        WorkingSet ws;

        CollectionScanParams params;
        params.direction = direction;
        params.tailable = false;

        unique_ptr<CollectionScan> scan(new CollectionScan(&_opCtx, collection, params, &ws, NULL));
        while (!scan->isEOF()) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState state = scan->work(&id);
            if (PlanStage::ADVANCED == state) {
                WorkingSetMember* member = ws.get(id);
                verify(member->hasRecordId());
                out->push_back(member->recordId);
            }
        }
    }

    unique_ptr<CanonicalQuery> canonicalize(const BSONObj& query) {
        auto qr = std::make_unique<QueryRequest>(nss);
        qr->setFilter(query);
        auto statusWithCQ = CanonicalQuery::canonicalize(&_opCtx, std::move(qr));
        ASSERT_OK(statusWithCQ.getStatus());
        return std::move(statusWithCQ.getValue());
    }

    static size_t numObj() {
        return 50;
    }

protected:
    const ServiceContext::UniqueOperationContext _txnPtr = cc().makeOperationContext();
    OperationContext& _opCtx = *_txnPtr;

private:
    DBDirectClient _client;
};

// Use the delete stage to delete some objects retrieved by a collscan, then separately delete the
// upcoming object. We expect the delete stage to skip over it and successfully continue.
class QueryStageDeleteUpcomingObjectWasDeleted : public QueryStageDeleteBase {
public:
    void run() {
        dbtests::WriteContextForTests ctx(&_opCtx, nss.ns());

        Collection* coll = ctx.getCollection();
        ASSERT(coll);

        // Get the RecordIds that would be returned by an in-order scan.
        vector<RecordId> recordIds;
        getRecordIds(coll, CollectionScanParams::FORWARD, &recordIds);

        // Configure the scan.
        CollectionScanParams collScanParams;
        collScanParams.direction = CollectionScanParams::FORWARD;
        collScanParams.tailable = false;

        // Configure the delete stage.
        auto deleteStageParams = std::make_unique<DeleteStageParams>();
        deleteStageParams->isMulti = true;

        WorkingSet ws;
        DeleteStage deleteStage(&_opCtx,
                                std::move(deleteStageParams),
                                &ws,
                                coll,
                                new CollectionScan(&_opCtx, coll, collScanParams, &ws, NULL));

        const DeleteStats* stats = static_cast<const DeleteStats*>(deleteStage.getSpecificStats());

        const size_t targetDocIndex = 10;

        while (stats->docsDeleted < targetDocIndex) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState state = deleteStage.work(&id);
            ASSERT_EQUALS(PlanStage::NEED_TIME, state);
        }

        // Remove recordIds[targetDocIndex];
        static_cast<PlanStage*>(&deleteStage)->saveState();
        BSONObj targetDoc = coll->docFor(&_opCtx, recordIds[targetDocIndex]).value();
        ASSERT(!targetDoc.isEmpty());
        remove(targetDoc);
        static_cast<PlanStage*>(&deleteStage)->restoreState();

        // Remove the rest.
        while (!deleteStage.isEOF()) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState state = deleteStage.work(&id);
            invariant(PlanStage::NEED_TIME == state || PlanStage::IS_EOF == state);
        }

        ASSERT_EQUALS(numObj() - 1, stats->docsDeleted);
    }
};

/**
 * Test that the delete stage returns an owned copy of the original document if returnDeleted is
 * specified.
 */
class QueryStageDeleteReturnOldDoc : public QueryStageDeleteBase {
public:
    void run() {
        // Various variables we'll need.
        dbtests::WriteContextForTests ctx(&_opCtx, nss.ns());
        Collection* coll = ctx.getCollection();
        ASSERT(coll);
        const int targetDocIndex = 0;
        const BSONObj query = BSON("foo" << BSON("$gte" << targetDocIndex));
        const auto ws = std::make_unique<WorkingSet>();
        const unique_ptr<CanonicalQuery> cq(canonicalize(query));

        // Get the RecordIds that would be returned by an in-order scan.
        vector<RecordId> recordIds;
        getRecordIds(coll, CollectionScanParams::FORWARD, &recordIds);

        // Configure a QueuedDataStage to pass the first object in the collection back in a
        // RID_AND_OBJ state.
        auto qds = std::make_unique<QueuedDataStage>(&_opCtx, ws.get());
        WorkingSetID id = ws->allocate();
        WorkingSetMember* member = ws->get(id);
        member->recordId = recordIds[targetDocIndex];
        const BSONObj oldDoc = BSON("_id" << targetDocIndex << "foo" << targetDocIndex);
        member->obj = Snapshotted<BSONObj>(SnapshotId(), oldDoc);
        ws->transitionToRecordIdAndObj(id);
        qds->pushBack(id);

        // Configure the delete.
        auto deleteParams = std::make_unique<DeleteStageParams>();
        deleteParams->returnDeleted = true;
        deleteParams->canonicalQuery = cq.get();

        const auto deleteStage = std::make_unique<DeleteStage>(
            &_opCtx, std::move(deleteParams), ws.get(), coll, qds.release());

        const DeleteStats* stats = static_cast<const DeleteStats*>(deleteStage->getSpecificStats());

        // Should return advanced.
        id = WorkingSet::INVALID_ID;
        PlanStage::StageState state = deleteStage->work(&id);
        ASSERT_EQUALS(PlanStage::ADVANCED, state);

        // Make sure the returned value is what we expect it to be.

        // Should give us back a valid id.
        ASSERT_TRUE(WorkingSet::INVALID_ID != id);
        WorkingSetMember* resultMember = ws->get(id);
        // With an owned copy of the object, with no RecordId.
        ASSERT_TRUE(resultMember->hasOwnedObj());
        ASSERT_FALSE(resultMember->hasRecordId());
        ASSERT_EQUALS(resultMember->getState(), WorkingSetMember::OWNED_OBJ);
        ASSERT_TRUE(resultMember->obj.value().isOwned());

        // Should be the old value.
        ASSERT_BSONOBJ_EQ(resultMember->obj.value(), oldDoc);

        // Should have done the delete.
        ASSERT_EQUALS(stats->docsDeleted, 1U);
        // That should be it.
        id = WorkingSet::INVALID_ID;
        ASSERT_EQUALS(PlanStage::IS_EOF, deleteStage->work(&id));
    }
};

class All : public Suite {
public:
    All() : Suite("query_stage_delete") {}

    void setupTests() {
        // Stage-specific tests below.
        add<QueryStageDeleteUpcomingObjectWasDeleted>();
        add<QueryStageDeleteReturnOldDoc>();
    }
};

SuiteInstance<All> all;

}  // namespace QueryStageDelete
