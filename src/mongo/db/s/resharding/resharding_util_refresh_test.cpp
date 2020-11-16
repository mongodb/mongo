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

#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/s/config/config_server_test_fixture.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/shard_id.h"

namespace mongo {
namespace {

const std::vector<ShardId> kShardIdList{{"s1"}, {"s2"}};
const std::vector<ShardType> kShards{{"s1", "s1:123"}, {"s2", "s2:123"}};

const Status kMockStatus = {ErrorCodes::InternalError, "test"};
const BSONObj kMockErrorRes = BSON("ok" << 0 << "code" << kMockStatus.code());

const BSONObj kMockWriteConcernError = BSON("code" << ErrorCodes::WriteConcernFailed << "errmsg"
                                                   << "Mock");
const BSONObj kMockResWithWriteConcernError =
    BSON("ok" << 1 << "writeConcernError" << kMockWriteConcernError);

const Status kRetryableError{ErrorCodes::HostUnreachable, "RetryableError for test"};

class ReshardingRefresherTest : public ConfigServerTestFixture {
protected:
    void setUp() {
        ConfigServerTestFixture::setUp();

        for (const auto& shard : kShards) {
            targeterFactory()->addTargeterToReturn(
                ConnectionString(HostAndPort{shard.getHost()}), [&] {
                    auto targeter = std::make_unique<RemoteCommandTargeterMock>();
                    targeter->setFindHostReturnValue(HostAndPort{shard.getHost()});
                    return targeter;
                }());
        }

        setupShards(kShards);
        shardRegistry()->reload(operationContext());
    }
};

TEST_F(ReshardingRefresherTest, refresherTwoShardsSucceed) {
    auto opCtx = operationContext();
    auto nss = NamespaceString("mydb", "mycoll");
    auto future = launchAsync([&] { tellShardsToRefresh(opCtx, kShardIdList, nss, executor()); });

    onCommand([&](const executor::RemoteCommandRequest& request) { return BSON("ok" << 1); });
    onCommand([&](const executor::RemoteCommandRequest& request) { return BSON("ok" << 1); });

    future.default_timed_get();
}

TEST_F(ReshardingRefresherTest, refresherTwoShardsFirstErrors) {
    auto opCtx = operationContext();
    auto nss = NamespaceString("mydb", "mycoll");
    auto future = launchAsync([&] { tellShardsToRefresh(opCtx, kShardIdList, nss, executor()); });

    onCommand([&](const executor::RemoteCommandRequest& request) { return kMockErrorRes; });

    ASSERT_THROWS_CODE(future.default_timed_get(), DBException, kMockStatus.code());
}

TEST_F(ReshardingRefresherTest, refresherTwoShardsSecondErrors) {
    auto opCtx = operationContext();
    auto nss = NamespaceString("mydb", "mycoll");
    auto future = launchAsync([&] { tellShardsToRefresh(opCtx, kShardIdList, nss, executor()); });

    onCommand([&](const executor::RemoteCommandRequest& request) { return BSON("ok" << 1); });
    onCommand([&](const executor::RemoteCommandRequest& request) { return kMockErrorRes; });

    ASSERT_THROWS_CODE(future.default_timed_get(), DBException, kMockStatus.code());
}

TEST_F(ReshardingRefresherTest, refresherTwoShardsWriteConcernFailed) {
    auto opCtx = operationContext();
    auto nss = NamespaceString("mydb", "mycoll");
    auto future = launchAsync([&] { tellShardsToRefresh(opCtx, kShardIdList, nss, executor()); });

    onCommand([&](const executor::RemoteCommandRequest& request) {
        return kMockResWithWriteConcernError;
    });

    ASSERT_THROWS_CODE(future.default_timed_get(), DBException, ErrorCodes::WriteConcernFailed);
}

}  // namespace
}  // namespace mongo
