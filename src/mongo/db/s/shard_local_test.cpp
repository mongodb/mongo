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

#include <boost/move/utility_core.hpp>
#include <fmt/format.h>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/client.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/s/shard_local.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/storage/snapshot_manager.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo {
namespace {

class ShardLocalTest : public ServiceContextMongoDTest {
protected:
    ShardLocalTest() {
        serverGlobalParams.clusterRole = {
            ClusterRole::ShardServer, ClusterRole::ConfigServer, ClusterRole::RouterServer};
    }

    ~ShardLocalTest() {
        serverGlobalParams.clusterRole = ClusterRole::None;
    }

    void setUp() override {
        ServiceContextMongoDTest::setUp();
        _opCtx = getGlobalServiceContext()->makeOperationContext(&cc());
        _shardLocal = std::make_unique<ShardLocal>(ShardId::kConfigServerId);
        const repl::ReplSettings replSettings = {};
        repl::ReplicationCoordinator::set(
            getGlobalServiceContext(),
            std::unique_ptr<repl::ReplicationCoordinator>(
                new repl::ReplicationCoordinatorMock(_opCtx->getServiceContext(), replSettings)));
        ASSERT_OK(repl::ReplicationCoordinator::get(getGlobalServiceContext())
                      ->setFollowerMode(repl::MemberState::RS_PRIMARY));

        repl::createOplog(_opCtx.get());

        // Set a committed snapshot so that we can perform majority reads.
        WriteUnitOfWork wuow{_opCtx.get()};
        _opCtx->getServiceContext()->getStorageEngine()->getSnapshotManager()->setCommittedSnapshot(
            repl::getNextOpTime(_opCtx.get()).getTimestamp());
        wuow.commit();
    }

    void tearDown() override {
        _opCtx.reset();
        ServiceContextMongoDTest::tearDown();
        repl::ReplicationCoordinator::set(getGlobalServiceContext(), nullptr);
    }

    /**
     * Sets up and runs a FindAndModify command with ShardLocal's runCommand. Finds a document in
     * namespace "nss" that matches "find" and updates the document with "set". Upsert and new are
     * set to true in the FindAndModify request.
     */
    StatusWith<Shard::CommandResponse> runFindAndModifyRunCommand(NamespaceString nss,
                                                                  BSONObj find,
                                                                  BSONObj set) {
        auto findAndModifyRequest = write_ops::FindAndModifyCommandRequest(nss);
        findAndModifyRequest.setQuery(find);
        findAndModifyRequest.setUpdate(write_ops::UpdateModification::parseFromClassicUpdate(set));
        findAndModifyRequest.setUpsert(true);
        findAndModifyRequest.setNew(true);
        findAndModifyRequest.setWriteConcern(
            WriteConcernOptions(
                WriteConcernOptions::kMajority, WriteConcernOptions::SyncMode::UNSET, Seconds(15))
                .toBSON());

        return _shardLocal->runCommandWithFixedRetryAttempts(
            _opCtx.get(),
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            nss.dbName(),
            findAndModifyRequest.toBSON({}),
            Shard::RetryPolicy::kNoRetry);
    }
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
    StatusWith<std::vector<BSONObj>> getIndexes(NamespaceString nss) {
        auto response = _shardLocal->runCommandWithFixedRetryAttempts(
            _opCtx.get(),
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            nss.dbName(),
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

    service_context_test::ShardRoleOverride _shardRole;

    ServiceContext::UniqueOperationContext _opCtx;
    std::unique_ptr<ShardLocal> _shardLocal;
};

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
    return _shardLocal->exhaustiveFindOnConfig(_opCtx.get(),
                                               ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                               repl::ReadConcernLevel::kMajorityReadConcern,
                                               nss,
                                               query,
                                               sort,
                                               limit);
}

TEST_F(ShardLocalTest, RunCommand) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("admin.bar");
    StatusWith<Shard::CommandResponse> findAndModifyResponse = runFindAndModifyRunCommand(
        nss, BSON("fooItem" << 1), BSON("$set" << BSON("fooRandom" << 254)));

    Shard::CommandResponse commandResponse = unittest::assertGet(findAndModifyResponse);
    BSONObj newDocument = extractFindAndModifyNewObj(commandResponse.response);

    ASSERT_EQUALS(1, newDocument["fooItem"].numberInt());
    ASSERT_EQUALS(254, newDocument["fooRandom"].numberInt());
}

TEST_F(ShardLocalTest, FindOneWithoutLimit) {
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("admin.bar");

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
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("admin.bar");

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
    NamespaceString nss = NamespaceString::createNamespaceString_forTest("admin.bar");

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

}  // namespace
}  // namespace mongo
