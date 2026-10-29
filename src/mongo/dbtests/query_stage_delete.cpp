// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/classic/collection_scan.h"
#include "mongo/db/exec/classic/delete_stage.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/queued_data_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/collection_scan_common.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/record_id.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace QueryStageDelete {

static const NamespaceString nss =
    NamespaceString::createNamespaceString_forTest("unittests.QueryStageDelete");

//
// Stage-specific tests.
//

class QueryStageDeleteBase {
public:
    QueryStageDeleteBase() : _client(&_opCtx) {
        dbtests::WriteContextForTests ctx(&_opCtx, nss.ns_forTest());

        for (size_t i = 0; i < numObj(); ++i) {
            BSONObjBuilder bob;
            bob.append("_id", static_cast<long long int>(i));
            bob.append("foo", static_cast<long long int>(i));
            _client.insert(nss, bob.obj());
        }
    }

    virtual ~QueryStageDeleteBase() {
        _client.dropCollection(nss);
    }

    void remove(const BSONObj& obj) {
        _client.remove(nss, obj);
    }

    void getRecordIds(const CollectionAcquisition& collection,
                      CollectionScanParams::Direction direction,
                      std::vector<RecordId>* out) {
        WorkingSet ws;

        CollectionScanParams params;
        params.direction = direction;
        params.tailable = false;

        std::unique_ptr<CollectionScan> scan(
            new CollectionScan(_expCtx.get(), collection, params, &ws, nullptr));
        while (!scan->isEOF()) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState state = scan->work(&id);
            if (PlanStage::ADVANCED == state) {
                WorkingSetMember* member = ws.get(id);
                MONGO_verify(member->hasRecordId());
                out->push_back(member->recordId);
            }
        }
    }

    std::unique_ptr<CanonicalQuery> canonicalize(const BSONObj& query) {
        auto findCommand = std::make_unique<FindCommandRequest>(nss);
        findCommand->setFilter(query);
        return std::make_unique<CanonicalQuery>(CanonicalQueryParams{
            .expCtx = ExpressionContextBuilder{}.fromRequest(&_opCtx, *findCommand).build(),
            .parsedFind = ParsedFindCommandParams{std::move(findCommand)}});
    }

    static size_t numObj() {
        return 50;
    }

protected:
    const ServiceContext::UniqueOperationContext _txnPtr = cc().makeOperationContext();
    OperationContext& _opCtx = *_txnPtr;

    boost::intrusive_ptr<ExpressionContext> _expCtx =
        ExpressionContextBuilder{}.opCtx(&_opCtx).ns(nss).build();

private:
    DBDirectClient _client;
};

// Use the delete stage to delete some objects retrieved by a collscan, then separately delete the
// upcoming object. We expect the delete stage to skip over it and successfully continue.
class QueryStageDeleteUpcomingObjectWasDeleted : public QueryStageDeleteBase {
public:
    void run() {
        const auto coll = acquireCollection(
            &_opCtx,
            CollectionAcquisitionRequest::fromOpCtx(&_opCtx, nss, AcquisitionPrerequisites::kWrite),
            MODE_IX);

        ASSERT(coll.exists());

        // Get the RecordIds that would be returned by an in-order scan.
        std::vector<RecordId> recordIds;
        getRecordIds(coll, CollectionScanParams::FORWARD, &recordIds);

        // Configure the scan.
        CollectionScanParams collScanParams;
        collScanParams.direction = CollectionScanParams::FORWARD;
        collScanParams.tailable = false;

        // Configure the delete stage.
        DeleteStageParams deleteStageParams;
        deleteStageParams.isMulti = true;

        WorkingSet ws;
        DeleteStage deleteStage(
            _expCtx.get(),
            std::move(deleteStageParams),
            &ws,
            coll,
            new CollectionScan(_expCtx.get(), coll, collScanParams, &ws, nullptr));

        const DeleteStats* stats = static_cast<const DeleteStats*>(deleteStage.getSpecificStats());

        const size_t targetDocIndex = 10;

        while (stats->docsDeleted < targetDocIndex) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            PlanStage::StageState state = deleteStage.work(&id);
            ASSERT_EQUALS(PlanStage::NEED_TIME, state);
        }

        // Remove recordIds[targetDocIndex];
        static_cast<PlanStage*>(&deleteStage)->saveState();
        BSONObj targetDoc =
            coll.getCollectionPtr()->docFor(&_opCtx, recordIds[targetDocIndex]).value();
        ASSERT(!targetDoc.isEmpty());
        remove(targetDoc);
        static_cast<PlanStage*>(&deleteStage)->restoreState(&coll.getCollectionPtr());

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
        const auto coll = acquireCollection(
            &_opCtx,
            CollectionAcquisitionRequest::fromOpCtx(&_opCtx, nss, AcquisitionPrerequisites::kWrite),
            MODE_IX);

        ASSERT(coll.exists());
        const int targetDocIndex = 0;
        const BSONObj query = BSON("foo" << BSON("$gte" << targetDocIndex));
        const auto ws = std::make_unique<WorkingSet>();
        const std::unique_ptr<CanonicalQuery> cq(canonicalize(query));

        // Get the RecordIds that would be returned by an in-order scan.
        std::vector<RecordId> recordIds;
        getRecordIds(coll, CollectionScanParams::FORWARD, &recordIds);

        // Configure a QueuedDataStage to pass the first object in the collection back in a
        // RID_AND_OBJ state.
        auto qds = std::make_unique<QueuedDataStage>(_expCtx.get(), ws.get());
        WorkingSetID id = ws->allocate();
        WorkingSetMember* member = ws->get(id);
        member->recordId = recordIds[targetDocIndex];
        const BSONObj oldDoc = BSON("_id" << targetDocIndex << "foo" << targetDocIndex);
        member->doc = {SnapshotId(), Document{oldDoc}};
        ws->transitionToRecordIdAndObj(id);
        qds->pushBack(id);

        // Configure the delete.
        DeleteStageParams deleteParams;
        deleteParams.returnDeleted = true;
        deleteParams.canonicalQuery = cq.get();

        const auto deleteStage = std::make_unique<DeleteStage>(
            _expCtx.get(), std::move(deleteParams), ws.get(), coll, qds.release());

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
        ASSERT_TRUE(resultMember->doc.value().isOwned());

        // Should be the old value.
        ASSERT_BSONOBJ_EQ(resultMember->doc.value().toBson(), oldDoc);

        // Should have done the delete.
        ASSERT_EQUALS(stats->docsDeleted, 1U);
        // That should be it.
        id = WorkingSet::INVALID_ID;
        ASSERT_EQUALS(PlanStage::IS_EOF, deleteStage->work(&id));
    }
};

class All : public unittest::OldStyleSuiteSpecification {
public:
    All() : OldStyleSuiteSpecification("query_stage_delete") {}

    void setupTests() override {
        // Stage-specific tests below.
        add<QueryStageDeleteUpcomingObjectWasDeleted>();
        add<QueryStageDeleteReturnOldDoc>();
    }
};

unittest::OldStyleSuiteInitializer<All> all;

}  // namespace QueryStageDelete
}  // namespace mongo
