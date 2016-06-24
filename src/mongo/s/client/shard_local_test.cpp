/**
 * Copyright (C) 2016 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/s/client/shard_local.h"

#include "mongo/client/read_preference.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/find_and_modify_request.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/stdx/memory.h"

namespace mongo {
namespace {

class ShardLocalTest : public ServiceContextMongoDTest {
protected:
    ServiceContext::UniqueOperationContext _txn;
    std::unique_ptr<ShardLocal> _shardLocal;

    /**
     * Sets up and runs a FindAndModify command with ShardLocal's runCommand. Finds a document in
     * namespace "nss" that matches "find" and updates the document with "set". Upsert and new are
     * set to true in the FindAndModify request.
     */
    StatusWith<Shard::CommandResponse> runFindAndModifyRunCommand(NamespaceString nss,
                                                                  BSONObj find,
                                                                  BSONObj set);
    /**
     * Facilitates running a find query by supplying the redundant parameters. Finds documents in
     * namespace "nss" that match "query" and returns "limit" (if there are that many) number of
     * documents in "sort" order.
     */
    StatusWith<Shard::QueryResponse> runFindQuery(NamespaceString nss,
                                                  BSONObj query,
                                                  BSONObj sort,
                                                  boost::optional<long long> limit);

    /**
     * Returns the index definitions that exist for the given collection.
     */
    StatusWith<std::vector<BSONObj>> getIndexes(NamespaceString nss);

private:
    void setUp() override;
    void tearDown() override;
};

void ShardLocalTest::setUp() {
    ServiceContextMongoDTest::setUp();
    Client::initThreadIfNotAlready();
    _txn = getGlobalServiceContext()->makeOperationContext(&cc());
    serverGlobalParams.clusterRole = ClusterRole::ConfigServer;
    _shardLocal = stdx::make_unique<ShardLocal>(ShardId("shardOrConfig"));
    const repl::ReplSettings replSettings = {};
    repl::setGlobalReplicationCoordinator(new repl::ReplicationCoordinatorMock(replSettings));
}

void ShardLocalTest::tearDown() {
    _txn.reset();
    ServiceContextMongoDTest::tearDown();
    repl::setGlobalReplicationCoordinator(nullptr);
}

StatusWith<Shard::CommandResponse> ShardLocalTest::runFindAndModifyRunCommand(NamespaceString nss,
                                                                              BSONObj find,
                                                                              BSONObj set) {
    FindAndModifyRequest findAndModifyRequest = FindAndModifyRequest::makeUpdate(nss, find, set);
    findAndModifyRequest.setUpsert(true);
    findAndModifyRequest.setShouldReturnNew(true);
    findAndModifyRequest.setWriteConcern(WriteConcernOptions(
        WriteConcernOptions::kMajority, WriteConcernOptions::SyncMode::UNSET, Seconds(15)));

    return _shardLocal->runCommand(_txn.get(),
                                   ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                   nss.db().toString(),
                                   findAndModifyRequest.toBSON(),
                                   Shard::RetryPolicy::kNoRetry);
}

StatusWith<std::vector<BSONObj>> ShardLocalTest::getIndexes(NamespaceString nss) {
    auto response = _shardLocal->runCommand(_txn.get(),
                                            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                            nss.db().toString(),
                                            BSON("listIndexes" << nss.coll().toString()),
                                            Shard::RetryPolicy::kIdempotent);
    if (!response.isOK()) {
        return response.getStatus();
    }
    if (!response.getValue().commandStatus.isOK()) {
        return response.getValue().commandStatus;
    }

    auto cursorResponse = CursorResponse::parseFromBSON(response.getValue().response);
    if (!cursorResponse.isOK()) {
        return cursorResponse.getStatus();
    }
    return cursorResponse.getValue().getBatch();
}

/**
 * Takes a FindAndModify command's BSON response and parses it for the returned "value" field.
 */
BSONObj extractFindAndModifyNewObj(const BSONObj& responseObj) {
    const auto& newDocElem = responseObj["value"];
    ASSERT(!newDocElem.eoo());
    ASSERT(newDocElem.isABSONObj());
    return newDocElem.Obj();
}

StatusWith<Shard::QueryResponse> ShardLocalTest::runFindQuery(NamespaceString nss,
                                                              BSONObj query,
                                                              BSONObj sort,
                                                              boost::optional<long long> limit) {
    return _shardLocal->exhaustiveFindOnConfig(_txn.get(),
                                               ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                               repl::ReadConcernLevel::kMajorityReadConcern,
                                               nss,
                                               query,
                                               sort,
                                               limit);
}

TEST_F(ShardLocalTest, RunCommand) {
    NamespaceString nss("admin.bar");
    StatusWith<Shard::CommandResponse> findAndModifyResponse = runFindAndModifyRunCommand(
        nss, BSON("fooItem" << 1), BSON("$set" << BSON("fooRandom" << 254)));

    Shard::CommandResponse commandResponse = unittest::assertGet(findAndModifyResponse);
    BSONObj newDocument = extractFindAndModifyNewObj(commandResponse.response);

    ASSERT_EQUALS(1, newDocument["fooItem"].numberInt());
    ASSERT_EQUALS(254, newDocument["fooRandom"].numberInt());
}

TEST_F(ShardLocalTest, FindOneWithoutLimit) {
    NamespaceString nss("admin.bar");

    // Set up documents to be queried.
    StatusWith<Shard::CommandResponse> findAndModifyResponse = runFindAndModifyRunCommand(
        nss, BSON("fooItem" << 1), BSON("$set" << BSON("fooRandom" << 254)));
    ASSERT_OK(findAndModifyResponse.getStatus());
    findAndModifyResponse = runFindAndModifyRunCommand(
        nss, BSON("fooItem" << 3), BSON("$set" << BSON("fooRandom" << 452)));
    ASSERT_OK(findAndModifyResponse.getStatus());

    // Find a single document.
    StatusWith<Shard::QueryResponse> response =
        runFindQuery(nss, BSON("fooItem" << 3), BSONObj(), boost::none);
    Shard::QueryResponse queryResponse = unittest::assertGet(response);

    std::vector<BSONObj> docs = queryResponse.docs;
    const unsigned long size = 1;
    ASSERT_EQUALS(size, docs.size());
    BSONObj foundDoc = docs[0];
    ASSERT_EQUALS(3, foundDoc["fooItem"].numberInt());
    ASSERT_EQUALS(452, foundDoc["fooRandom"].numberInt());
}

TEST_F(ShardLocalTest, FindManyWithLimit) {
    NamespaceString nss("admin.bar");

    // Set up documents to be queried.
    StatusWith<Shard::CommandResponse> findAndModifyResponse = runFindAndModifyRunCommand(
        nss, BSON("fooItem" << 1), BSON("$set" << BSON("fooRandom" << 254)));
    ASSERT_OK(findAndModifyResponse.getStatus());
    findAndModifyResponse = runFindAndModifyRunCommand(
        nss, BSON("fooItem" << 2), BSON("$set" << BSON("fooRandom" << 444)));
    ASSERT_OK(findAndModifyResponse.getStatus());
    findAndModifyResponse = runFindAndModifyRunCommand(
        nss, BSON("fooItem" << 3), BSON("$set" << BSON("fooRandom" << 452)));
    ASSERT_OK(findAndModifyResponse.getStatus());

    // Find 2 of 3 documents.
    StatusWith<Shard::QueryResponse> response =
        runFindQuery(nss, BSONObj(), BSON("fooItem" << 1), 2LL);
    Shard::QueryResponse queryResponse = unittest::assertGet(response);

    std::vector<BSONObj> docs = queryResponse.docs;
    const unsigned long size = 2;
    ASSERT_EQUALS(size, docs.size());
    BSONObj firstDoc = docs[0];
    ASSERT_EQUALS(1, firstDoc["fooItem"].numberInt());
    ASSERT_EQUALS(254, firstDoc["fooRandom"].numberInt());
    BSONObj secondDoc = docs[1];
    ASSERT_EQUALS(2, secondDoc["fooItem"].numberInt());
    ASSERT_EQUALS(444, secondDoc["fooRandom"].numberInt());
}

TEST_F(ShardLocalTest, FindNoMatchingDocumentsEmpty) {
    NamespaceString nss("admin.bar");

    // Set up a document.
    StatusWith<Shard::CommandResponse> findAndModifyResponse = runFindAndModifyRunCommand(
        nss, BSON("fooItem" << 1), BSON("$set" << BSON("fooRandom" << 254)));
    ASSERT_OK(findAndModifyResponse.getStatus());

    // Run a query that won't find any results.
    StatusWith<Shard::QueryResponse> response =
        runFindQuery(nss, BSON("fooItem" << 3), BSONObj(), boost::none);
    Shard::QueryResponse queryResponse = unittest::assertGet(response);

    std::vector<BSONObj> docs = queryResponse.docs;
    const unsigned long size = 0;
    ASSERT_EQUALS(size, docs.size());
}

TEST_F(ShardLocalTest, CreateIndex) {
    NamespaceString nss("config.foo");

    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound, getIndexes(nss).getStatus());

    Status status =
        _shardLocal->createIndexOnConfig(_txn.get(), nss, BSON("a" << 1 << "b" << 1), true);
    // Creating the index should implicitly create the collection
    ASSERT_OK(status);

    auto indexes = unittest::assertGet(getIndexes(nss));
    // There should be the index we just added as well as the _id index
    ASSERT_EQ(2U, indexes.size());

    // Making an identical index should be a no-op.
    status = _shardLocal->createIndexOnConfig(_txn.get(), nss, BSON("a" << 1 << "b" << 1), true);
    ASSERT_OK(status);
    indexes = unittest::assertGet(getIndexes(nss));
    ASSERT_EQ(2U, indexes.size());

    // Trying to make the same index as non-unique should fail.
    status = _shardLocal->createIndexOnConfig(_txn.get(), nss, BSON("a" << 1 << "b" << 1), false);
    ASSERT_EQUALS(ErrorCodes::IndexOptionsConflict, status);
    indexes = unittest::assertGet(getIndexes(nss));
    ASSERT_EQ(2U, indexes.size());
}

}  // namespace
}  // namespace mongo
