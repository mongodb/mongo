/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/db/write_concern.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/unittest/unittest.h"

using namespace fmt::literals;

namespace mongo {
namespace {

class SimpleClient {
public:
    SimpleClient(OperationContext* opCtx) : _client(opCtx), _opCtx(opCtx) {}

    Status insert(const NamespaceString& nss, const BSONObj& doc) {
        const auto commandResponse = _client.runCommand([&] {
            write_ops::InsertCommandRequest insertOp(nss);
            insertOp.setDocuments({doc});
            return insertOp.serialize({});
        }());

        auto status = getStatusFromWriteCommandReply(commandResponse->getCommandReply());

        WriteConcernResult ignoreResult;
        auto latestOpTime = repl::ReplClientInfo::forClient(_opCtx->getClient()).getLastOp();
        uassertStatusOK(
            waitForWriteConcern(_opCtx, latestOpTime, kMajorityWriteConcern, &ignoreResult));

        return status;
    }

private:
    const WriteConcernOptions kMajorityWriteConcern{
        WriteConcernOptions::kMajority,
        WriteConcernOptions::SyncMode::UNSET,
        WriteConcernOptions::kWriteConcernTimeoutSharding};

    DBDirectClient _client;
    OperationContext* _opCtx;
};

using ReshardingCollectionTest = ShardServerTestFixture;

TEST_F(ReshardingCollectionTest, TestWritesToTempReshardingCollection) {
    SimpleClient client(operationContext());
    auto uuid = UUID::gen();

    auto tempNss = NamespaceString{"{}.system.resharding.{}"_format("test", uuid.toString())};
    ASSERT_OK(client.insert(tempNss, BSON("x" << 5)));

    auto chunksNss = NamespaceString{
        "config.cache.chunks.{}.system.resharding.{}"_format("test", uuid.toString())};
    ASSERT_OK(client.insert(chunksNss, BSON("X" << 5)));
}

}  // namespace
}  // namespace mongo
