// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/classic/collection_scan.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/collection_scan_common.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/unittest.h"

#include <memory>

#include <boost/intrusive_ptr.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

class CollectionScanFloorTest : public ServiceContextMongoDTest {
public:
    Options makeServiceContextOptions() override {
        return ServiceContextMongoDTest::makeServiceContextOptions().engine("wiredTiger");
    }

protected:
    void setUp() override {
        ServiceContextMongoDTest::setUp();
        _opCtx = makeOperationContext();

        auto replCoord = repl::ReplicationCoordinator::get(_opCtx.get());
        auto replCoordMock = dynamic_cast<repl::ReplicationCoordinatorMock*>(replCoord);
        ASSERT(replCoordMock);
        ASSERT_OK(replCoordMock->setFollowerMode(repl::MemberState::RS_PRIMARY));

        repl::createOplog(_opCtx.get());
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

    void insertOplogEntry(Timestamp ts, BSONObj entry) {
        WriteUnitOfWork wuow(opCtx());
        AutoGetCollection oplog(opCtx(), NamespaceString::kRsOplogNamespace, MODE_IX);
        InsertStatement insert(entry);
        insert.replicatedRecordId = RecordId(ts.asULL());
        ASSERT_OK(collection_internal::insertDocument(opCtx(), *oplog, insert, nullptr));
        wuow.commit();
    }

    // The ordinary first entry of a truncated oplog.
    void insertOplogEntryAfterTruncation(Timestamp ts) {
        insertOplogEntry(ts,
                         BSON("ts" << ts << "t" << 1LL << "op"
                                   << "i"
                                   << "ns"
                                   << "test.coll"
                                   << "o" << BSON("_id" << 1)));
    }

    // The first entry of an oplog that has never been truncated.
    void insertReplSetInitiationEntry(Timestamp ts) {
        insertOplogEntry(ts,
                         BSON("ts" << ts << "t" << 1LL << "op"
                                   << "n"
                                   << "ns"
                                   << ""
                                   << "o"
                                   << BSON(repl::kNewPrimaryMsgField << repl::kInitiatingSetMsg)));
    }

    boost::intrusive_ptr<ExpressionContext> makeExpCtx(bool showErrorsAsEvents) {
        auto expCtx = ExpressionContextBuilder{}
                          .opCtx(opCtx())
                          .ns(NamespaceString::kRsOplogNamespace)
                          .build();
        if (showErrorsAsEvents) {
            DocumentSourceChangeStreamSpec spec;
            spec.setShowErrorsAsEvents(true);
            expCtx->setChangeStreamSpec(spec);
        }
        return expCtx;
    }

    // Runs an oplog scan that asserts 'tsToCheck' has not fallen off the oplog until it produces a
    // document or reaches EOF. Returns the final state.
    PlanStage::StageState runScan(ExpressionContext* expCtx, Timestamp tsToCheck) {
        const auto coll = acquireCollection(
            opCtx(),
            CollectionAcquisitionRequest(NamespaceString::kRsOplogNamespace,
                                         PlacementConcern(boost::none, ShardVersion::UNTRACKED()),
                                         repl::ReadConcernArgs::get(opCtx()),
                                         AcquisitionPrerequisites::kRead),
            MODE_IS);

        CollectionScanParams params;
        params.direction = CollectionScanParams::FORWARD;
        params.shouldTrackLatestOplogTimestamp = true;
        params.assertTsHasNotFallenOff = tsToCheck;

        WorkingSet ws;
        CollectionScan scan(expCtx, coll, params, &ws, nullptr);
        PlanStage::StageState state = PlanStage::NEED_TIME;
        while (state == PlanStage::NEED_TIME) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            state = scan.work(&id);
        }
        return state;
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(CollectionScanFloorTest, CollectionScanFloorOptedInRecordsFloorAndContinues) {
    insertOplogEntryAfterTruncation(Timestamp(10, 1));
    auto expCtx = makeExpCtx(true /* showErrorsAsEvents */);

    ASSERT_EQ(runScan(expCtx.get(), Timestamp(5, 1)), PlanStage::ADVANCED);
    ASSERT_EQ(expCtx->getChangeStreamOplogFloorAfterRollover(), Timestamp(10, 1));
}

TEST_F(CollectionScanFloorTest, CollectionScanFloorNotOptedInThrowsAndRecordsNothing) {
    insertOplogEntryAfterTruncation(Timestamp(10, 1));
    auto expCtx = makeExpCtx(false /* showErrorsAsEvents */);

    ASSERT_THROWS_CODE(
        runScan(expCtx.get(), Timestamp(5, 1)), DBException, ErrorCodes::OplogQueryMinTsMissing);
    ASSERT_FALSE(expCtx->getChangeStreamOplogFloorAfterRollover());
}

TEST_F(CollectionScanFloorTest, CollectionScanFloorNewReplicaSetOptedInRecordsNothing) {
    insertReplSetInitiationEntry(Timestamp(10, 1));
    auto expCtx = makeExpCtx(true /* showErrorsAsEvents */);

    ASSERT_EQ(runScan(expCtx.get(), Timestamp(5, 1)), PlanStage::ADVANCED);
    ASSERT_FALSE(expCtx->getChangeStreamOplogFloorAfterRollover());
}

TEST_F(CollectionScanFloorTest, CollectionScanFloorNewReplicaSetNotOptedInRecordsNothing) {
    insertReplSetInitiationEntry(Timestamp(10, 1));
    auto expCtx = makeExpCtx(false /* showErrorsAsEvents */);

    ASSERT_EQ(runScan(expCtx.get(), Timestamp(5, 1)), PlanStage::ADVANCED);
    ASSERT_FALSE(expCtx->getChangeStreamOplogFloorAfterRollover());
}

}  // namespace
}  // namespace mongo
