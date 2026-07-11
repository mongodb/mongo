// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/start_chunk_clone_request.h"

#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <string>

#include <fmt/format.h>

namespace mongo {

using unittest::assertGet;

namespace {

TEST(StartChunkCloneRequest, CreateAsCommandComplete) {
    auto serviceContext = ServiceContext::make();
    auto client = serviceContext->getService()->makeClient("TestClient");
    auto opCtx = client->makeOperationContext();

    MigrationSessionId sessionId = MigrationSessionId::generate("shard0001", "shard0002");
    UUID migrationId = UUID::gen();
    auto lsid = makeLogicalSessionId(opCtx.get());
    TxnNumber txnNumber = 0;

    BSONObjBuilder builder;
    StartChunkCloneRequest::appendAsCommand(
        &builder,
        NamespaceString::createNamespaceString_forTest("TestDB.TestColl"),
        migrationId,
        lsid,
        txnNumber,
        sessionId,
        assertGet(ConnectionString::parse("TestDonorRS/Donor1:12345,Donor2:12345,Donor3:12345")),
        ShardId("shard0001"),
        ShardId("shard0002"),
        BSON("Key" << -100),
        BSON("Key" << 100),
        BSON("Key" << 1),
        MigrationSecondaryThrottleOptions::create(MigrationSecondaryThrottleOptions::kOff),
        ChunkRange(BSON("Key" << -200), BSON("Key" << 200)),
        true /* isAuthoritative */);

    BSONObj cmdObj = builder.obj();

    auto request = assertGet(StartChunkCloneRequest::createFromCommand(
        NamespaceString::createNamespaceString_forTest(cmdObj["_recvChunkStart"].String()),
        cmdObj));

    ASSERT_EQ("TestDB.TestColl", request.getNss().ns_forTest());
    ASSERT_EQ(sessionId.toString(), request.getSessionId().toString());
    ASSERT_EQ(migrationId, request.getMigrationId());
    ASSERT_EQ(lsid, request.getLsid());
    ASSERT_EQ(txnNumber, request.getTxnNumber());
    ASSERT(sessionId.matches(request.getSessionId()));
    ASSERT_EQ(
        assertGet(ConnectionString::parse("TestDonorRS/Donor1:12345,Donor2:12345,Donor3:12345"))
            .toString(),
        request.getFromShardConnectionString().toString());
    ASSERT_EQ("shard0001", request.getFromShardId().toString());
    ASSERT_EQ("shard0002", request.getToShardId().toString());
    ASSERT_BSONOBJ_EQ(BSON("Key" << -100), request.getMinKey());
    ASSERT_BSONOBJ_EQ(BSON("Key" << 100), request.getMaxKey());
    ASSERT_TRUE(request.getEnclosingChunk().has_value());
    ASSERT_BSONOBJ_EQ(BSON("Key" << -200), request.getEnclosingChunk()->getMin());
    ASSERT_BSONOBJ_EQ(BSON("Key" << 200), request.getEnclosingChunk()->getMax());
    ASSERT_BSONOBJ_EQ(BSON("Key" << 1), request.getShardKeyPattern());
    ASSERT_EQ(MigrationSecondaryThrottleOptions::kOff,
              request.getSecondaryThrottle().getSecondaryThrottle());
    ASSERT_TRUE(request.isAuthoritative());
}

TEST(StartChunkCloneRequest, IsAuthoritativeRoundTrip) {
    auto serviceContext = ServiceContext::make();
    auto client = serviceContext->getService()->makeClient("TestClient");
    auto opCtx = client->makeOperationContext();

    for (bool isAuthoritative : {false, true}) {
        BSONObjBuilder builder;
        StartChunkCloneRequest::appendAsCommand(
            &builder,
            NamespaceString::createNamespaceString_forTest("TestDB.TestColl"),
            UUID::gen(),
            makeLogicalSessionId(opCtx.get()),
            0 /* txnNumber */,
            MigrationSessionId::generate("shard0001", "shard0002"),
            assertGet(ConnectionString::parse("TestDonorRS/Donor1:12345")),
            ShardId("shard0001"),
            ShardId("shard0002"),
            BSON("Key" << -100),
            BSON("Key" << 100),
            BSON("Key" << 1),
            MigrationSecondaryThrottleOptions::create(MigrationSecondaryThrottleOptions::kOff),
            boost::none /* enclosingChunk */,
            isAuthoritative);

        BSONObj cmdObj = builder.obj();
        auto request = assertGet(StartChunkCloneRequest::createFromCommand(
            NamespaceString::createNamespaceString_forTest(cmdObj["_recvChunkStart"].String()),
            cmdObj));

        ASSERT_EQ(isAuthoritative, request.isAuthoritative());
    }
}

// A donor that does not send the field (legacy / mixed-version path) must be treated as
// non-authoritative so the recipient keeps the pre-existing refresh behavior.
TEST(StartChunkCloneRequest, IsAuthoritativeDefaultsToFalseWhenAbsent) {
    auto serviceContext = ServiceContext::make();
    auto client = serviceContext->getService()->makeClient("TestClient");
    auto opCtx = client->makeOperationContext();

    BSONObjBuilder builder;
    StartChunkCloneRequest::appendAsCommand(
        &builder,
        NamespaceString::createNamespaceString_forTest("TestDB.TestColl"),
        UUID::gen(),
        makeLogicalSessionId(opCtx.get()),
        0 /* txnNumber */,
        MigrationSessionId::generate("shard0001", "shard0002"),
        assertGet(ConnectionString::parse("TestDonorRS/Donor1:12345")),
        ShardId("shard0001"),
        ShardId("shard0002"),
        BSON("Key" << -100),
        BSON("Key" << 100),
        BSON("Key" << 1),
        MigrationSecondaryThrottleOptions::create(MigrationSecondaryThrottleOptions::kOff),
        boost::none /* enclosingChunk */,
        true /* isAuthoritative */);

    // Drop the field to simulate a donor that does not send it.
    BSONObjBuilder strippedBuilder;
    for (auto&& elem : builder.obj()) {
        if (elem.fieldNameStringData() != "isAuthoritative") {
            strippedBuilder.append(elem);
        }
    }
    BSONObj cmdObj = strippedBuilder.obj();

    auto request = assertGet(StartChunkCloneRequest::createFromCommand(
        NamespaceString::createNamespaceString_forTest(cmdObj["_recvChunkStart"].String()),
        cmdObj));

    ASSERT_FALSE(request.isAuthoritative());
}

}  // namespace
}  // namespace mongo
