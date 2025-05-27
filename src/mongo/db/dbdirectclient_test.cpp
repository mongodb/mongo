/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/dbdirectclient.h"

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace mongo {
namespace {

auto& opCounters() {
    return serviceOpCounters(ClusterRole::ShardServer);
}

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
    auto globalDeletesCountBefore = opCounters().getDelete()->load();
    auto globalInsertsCountBefore = opCounters().getInsert()->load();
    auto globalUpdatesCountBefore = opCounters().getUpdate()->load();
    auto globalCommandsCountBefore = opCounters().getCommand()->load();
    DBDirectClient client(_opCtx);
    write_ops::InsertCommandRequest insertOp(kNs);
    insertOp.setDocuments({BSON("_id" << 1)});
    auto insertReply = client.insert(insertOp);
    ASSERT_EQ(insertReply.getN(), 1);
    ASSERT_FALSE(insertReply.getWriteErrors());
    auto globalCommandsCountAfter = opCounters().getCommand()->load();
    auto globalDeletesCountAfter = opCounters().getDelete()->load();
    auto globalInsertsCountAfter = opCounters().getInsert()->load();
    auto globalUpdatesCountAfter = opCounters().getUpdate()->load();
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
    auto globalDeletesCountBefore = opCounters().getDelete()->load();
    auto globalInsertsCountBefore = opCounters().getInsert()->load();
    auto globalUpdatesCountBefore = opCounters().getUpdate()->load();
    auto globalCommandsCountBefore = opCounters().getCommand()->load();
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
    auto globalCommandsCountAfter = opCounters().getCommand()->load();
    auto globalDeletesCountAfter = opCounters().getDelete()->load();
    auto globalInsertsCountAfter = opCounters().getInsert()->load();
    auto globalUpdatesCountAfter = opCounters().getUpdate()->load();
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

    auto globalDeletesCountBefore = opCounters().getDelete()->load();
    auto globalInsertsCountBefore = opCounters().getInsert()->load();
    auto globalUpdatesCountBefore = opCounters().getUpdate()->load();
    auto globalCommandsCountBefore = opCounters().getCommand()->load();
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
    auto globalCommandsCountAfter = opCounters().getCommand()->load();
    auto globalDeletesCountAfter = opCounters().getDelete()->load();
    auto globalInsertsCountAfter = opCounters().getInsert()->load();
    auto globalUpdatesCountAfter = opCounters().getUpdate()->load();
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
