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
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context_d_test_fixture.h"

namespace mongo {
namespace {

const NamespaceString kNs("a.b");

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
    DBDirectClient client(_opCtx);
    write_ops::InsertCommandRequest insertOp(kNs);
    insertOp.setDocuments({BSON("_id" << 1)});
    auto insertReply = client.insert(insertOp);
    ASSERT_EQ(insertReply.getN(), 1);
    ASSERT_FALSE(insertReply.getWriteErrors());
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

}  // namespace
}  // namespace mongo
