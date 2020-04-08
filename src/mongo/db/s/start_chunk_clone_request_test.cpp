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

#include "mongo/platform/basic.h"

#include "mongo/db/s/start_chunk_clone_request.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/logical_session_id_helpers.h"
#include "mongo/db/service_context.h"
#include "mongo/s/shard_id.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

using unittest::assertGet;

namespace {

TEST(StartChunkCloneRequest, CreateAsCommandComplete) {
    auto serviceContext = ServiceContext::make();
    auto client = serviceContext->makeClient("TestClient");
    auto opCtx = client->makeOperationContext();

    MigrationSessionId sessionId = MigrationSessionId::generate("shard0001", "shard0002");
    UUID migrationId = UUID::gen();
    auto lsid = makeLogicalSessionId(opCtx.get());
    TxnNumber txnNumber = 0;

    BSONObjBuilder builder;
    StartChunkCloneRequest::appendAsCommand(
        &builder,
        NamespaceString("TestDB.TestColl"),
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
        false);

    BSONObj cmdObj = builder.obj();

    auto request = assertGet(StartChunkCloneRequest::createFromCommand(
        NamespaceString(cmdObj["_recvChunkStart"].String()), cmdObj));

    ASSERT_EQ("TestDB.TestColl", request.getNss().ns());
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
    ASSERT_BSONOBJ_EQ(BSON("Key" << 1), request.getShardKeyPattern());
    ASSERT_EQ(MigrationSecondaryThrottleOptions::kOff,
              request.getSecondaryThrottle().getSecondaryThrottle());
}

}  // namespace
}  // namespace mongo
