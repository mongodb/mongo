// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/dbdirectclient.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/stats/counters.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"

#include <utility>
#include <vector>

#include <boost/cstdint.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {


const NamespaceString kNs = NamespaceString::createNamespaceString_forTest("a.b");

class DBDirectClientTest : public ServiceContextMongoDTest {
protected:
    void setUp() override {
        ServiceContextMongoDTest::setUp();
        const auto service = getServiceContext();
        auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(service);
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));

        repl::ReplicationCoordinator::set(service, std::move(replCoord));
        repl::createOplog(_opCtx);
    }

    ServiceContext::UniqueOperationContext _uniqueOpCtx{makeOperationContext()};
    OperationContext* _opCtx{_uniqueOpCtx.get()};
};

// Test that DBDirectClient is prevented from auth
TEST_F(DBDirectClientTest, NoAuth) {
    DBDirectClient client(_opCtx);
    BSONObj params;

    ASSERT_THROWS(client.auth(params), AssertionException);
}

TEST_F(DBDirectClientTest, InsertSingleDocumentSuccessful) {
    auto globalDeletesCountBefore = globalOpCounters().deletes->value();
    auto globalInsertsCountBefore = globalOpCounters().inserts->value();
    auto globalUpdatesCountBefore = globalOpCounters().updates->value();
    auto globalCommandsCountBefore = globalOpCounters().commands->value();
    DBDirectClient client(_opCtx);
    write_ops::InsertCommandRequest insertOp(kNs);
    insertOp.setDocuments({BSON("_id" << 1)});
    auto insertReply = client.insert(insertOp);
    ASSERT_EQ(insertReply.getN(), 1);
    ASSERT_FALSE(insertReply.getWriteErrors());
    auto globalCommandsCountAfter = globalOpCounters().commands->value();
    auto globalDeletesCountAfter = globalOpCounters().deletes->value();
    auto globalInsertsCountAfter = globalOpCounters().inserts->value();
    auto globalUpdatesCountAfter = globalOpCounters().updates->value();
    ASSERT_EQ(1, globalInsertsCountAfter - globalInsertsCountBefore);
    ASSERT_EQ(0, globalDeletesCountAfter - globalDeletesCountBefore);
    ASSERT_EQ(0, globalCommandsCountAfter - globalCommandsCountBefore);
    ASSERT_EQ(0, globalUpdatesCountAfter - globalUpdatesCountBefore);
}

TEST_F(DBDirectClientTest, InsertDuplicateDocumentDoesNotThrow) {
    DBDirectClient client(_opCtx);
    write_ops::InsertCommandRequest insertOp(kNs);
    insertOp.setDocuments({BSON("_id" << 1), BSON("_id" << 1)});
    auto insertReply = client.insert(insertOp);
    ASSERT_EQ(insertReply.getN(), 1);
    auto writeErrors = insertReply.getWriteErrors().value();
    ASSERT_EQ(writeErrors.size(), 1);
    ASSERT_EQ(writeErrors[0].getStatus(), ErrorCodes::DuplicateKey);
}

TEST_F(DBDirectClientTest, UpdateSingleDocumentSuccessfully) {
    auto globalDeletesCountBefore = globalOpCounters().deletes->value();
    auto globalInsertsCountBefore = globalOpCounters().inserts->value();
    auto globalUpdatesCountBefore = globalOpCounters().updates->value();
    auto globalCommandsCountBefore = globalOpCounters().commands->value();
    DBDirectClient client(_opCtx);
    write_ops::UpdateCommandRequest updateOp(kNs);
    updateOp.setUpdates({[&] {
        write_ops::UpdateOpEntry entry;
        entry.setQ(BSON("_id" << 1));
        entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(BSON("x" << 1)));
        entry.setUpsert(true);
        return entry;
    }()});
    auto updateReply = client.update(updateOp);
    // One upsert
    ASSERT_EQ(updateReply.getN(), 1);
    // No documents there initially
    ASSERT_EQ(updateReply.getNModified(), 0);
    ASSERT_FALSE(updateReply.getWriteErrors());
    auto globalCommandsCountAfter = globalOpCounters().commands->value();
    auto globalDeletesCountAfter = globalOpCounters().deletes->value();
    auto globalInsertsCountAfter = globalOpCounters().inserts->value();
    auto globalUpdatesCountAfter = globalOpCounters().updates->value();
    ASSERT_EQ(0, globalInsertsCountAfter - globalInsertsCountBefore);
    ASSERT_EQ(0, globalDeletesCountAfter - globalDeletesCountBefore);
    ASSERT_EQ(0, globalCommandsCountAfter - globalCommandsCountBefore);
    ASSERT_EQ(1, globalUpdatesCountAfter - globalUpdatesCountBefore);
}

TEST_F(DBDirectClientTest, UpdateDuplicateImmutableFieldDoesNotThrow) {
    DBDirectClient client(_opCtx);
    write_ops::UpdateCommandRequest updateOp(kNs);
    updateOp.setUpdates({[&] {
        write_ops::UpdateOpEntry entry;
        entry.setQ(BSON("_id" << 1));
        entry.setU(write_ops::UpdateModification::parseFromClassicUpdate(BSON("_id" << 2)));
        entry.setUpsert(true);
        return entry;
    }()});
    auto updateReply = client.update(updateOp);
    ASSERT_EQ(updateReply.getN(), 0);
    ASSERT_EQ(updateReply.getNModified(), 0);
    auto writeErrors = updateReply.getWriteErrors().value();
    ASSERT_EQ(writeErrors.size(), 1);
    ASSERT_EQ(writeErrors[0].getStatus(), ErrorCodes::ImmutableField);
}

TEST_F(DBDirectClientTest, DeleteSingleDocumentSuccessful) {
    DBDirectClient client(_opCtx);
    // Insert document to delete
    write_ops::InsertCommandRequest insertOp(kNs);
    insertOp.setDocuments({BSON("_id" << 1)});
    auto insertReply = client.insert(insertOp);

    auto globalDeletesCountBefore = globalOpCounters().deletes->value();
    auto globalInsertsCountBefore = globalOpCounters().inserts->value();
    auto globalUpdatesCountBefore = globalOpCounters().updates->value();
    auto globalCommandsCountBefore = globalOpCounters().commands->value();
    // Delete document
    write_ops::DeleteCommandRequest deleteOp(kNs);
    deleteOp.setDeletes({[&] {
        write_ops::DeleteOpEntry entry;
        entry.setQ(BSON("_id" << 1));
        entry.setMulti(false);
        return entry;
    }()});
    auto deleteReply = client.remove(deleteOp);
    ASSERT_EQ(deleteReply.getN(), 1);
    ASSERT_FALSE(deleteReply.getWriteErrors());
    auto globalCommandsCountAfter = globalOpCounters().commands->value();
    auto globalDeletesCountAfter = globalOpCounters().deletes->value();
    auto globalInsertsCountAfter = globalOpCounters().inserts->value();
    auto globalUpdatesCountAfter = globalOpCounters().updates->value();
    ASSERT_EQ(0, globalInsertsCountAfter - globalInsertsCountBefore);
    ASSERT_EQ(1, globalDeletesCountAfter - globalDeletesCountBefore);
    ASSERT_EQ(0, globalCommandsCountAfter - globalCommandsCountBefore);
    ASSERT_EQ(0, globalUpdatesCountAfter - globalUpdatesCountBefore);
}

TEST_F(DBDirectClientTest, DeleteDocumentIncorrectHintDoesNotThrow) {
    DBDirectClient client(_opCtx);
    // Insert document to delete
    write_ops::InsertCommandRequest insertOp(kNs);
    insertOp.setDocuments({BSON("_id" << 1)});
    auto insertReply = client.insert(insertOp);
    // Delete document
    write_ops::DeleteCommandRequest deleteOp(kNs);
    deleteOp.setDeletes({[&] {
        write_ops::DeleteOpEntry entry;
        entry.setQ(BSON("_id" << 1));
        entry.setMulti(false);
        entry.setHint(BSON("xyz" << 1));
        return entry;
    }()});
    auto deleteReply = client.remove(deleteOp);
    ASSERT_EQ(deleteReply.getN(), 0);
    auto writeErrors = deleteReply.getWriteErrors().value();
    ASSERT_EQ(writeErrors.size(), 1);
    ASSERT_EQ(writeErrors[0].getStatus(), ErrorCodes::BadValue);
}

TEST_F(DBDirectClientTest, ExhaustQuery) {
    DBDirectClient client(_opCtx);
    write_ops::InsertCommandRequest insertOp(kNs);
    const int numDocs = 10;
    std::vector<BSONObj> docsToInsert{numDocs};
    for (int i = 0; i < numDocs; ++i) {
        docsToInsert[i] = BSON("_id" << i);
    }
    insertOp.setDocuments(std::move(docsToInsert));
    auto insertReply = client.insert(insertOp);
    ASSERT_EQ(insertReply.getN(), numDocs);
    ASSERT_FALSE(insertReply.getWriteErrors());

    // The query should work even though exhaust mode is requested.
    FindCommandRequest findCmd{kNs};
    findCmd.setBatchSize(2);
    auto cursor = client.find(std::move(findCmd), ReadPreferenceSetting{}, ExhaustMode::kOn);
    ASSERT_EQ(cursor->itcount(), numDocs);
}

TEST_F(DBDirectClientTest, InternalErrorAllowedToEscapeDBDirectClient) {
    DBDirectClient client(_opCtx);
    FindCommandRequest findCmd{kNs};

    FailPointEnableBlock failPoint("failCommand",
                                   BSON("errorCode" << ErrorCodes::TransactionAPIMustRetryCommit
                                                    << "failCommands" << BSON_ARRAY("find")
                                                    << "failInternalCommands" << true
                                                    << "failLocalClients" << true));

    ASSERT_THROWS_CODE(client.find(std::move(findCmd), ReadPreferenceSetting{}, ExhaustMode::kOff),
                       DBException,
                       ErrorCodes::TransactionAPIMustRetryCommit);
}

}  // namespace
}  // namespace mongo
