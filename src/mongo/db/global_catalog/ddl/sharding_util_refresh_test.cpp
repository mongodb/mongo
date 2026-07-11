// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include <boost/move/utility_core.hpp>
// IWYU pragma: no_include "cxxabi.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/global_catalog/ddl/sharding_util.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/sharding_environment/config_server_test_fixture.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/hostandport.h"

#include <memory>
#include <string>
#include <system_error>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

const std::vector<ShardId> kShardIdList{{"s1"}, {"s2"}};
const std::vector<ShardType> kShards{{"s1", "s1:123"}, {"s2", "s2:123"}};

const Status kMockStatus = {ErrorCodes::InternalError, "test"};
const BSONObj kMockErrorRes = BSON("ok" << 0 << "code" << kMockStatus.code());

const BSONObj kMockWriteConcernError = BSON("code" << ErrorCodes::WriteConcernTimeout << "errmsg"
                                                   << "Mock");
const BSONObj kMockResWithWriteConcernError =
    BSON("ok" << 1 << "writeConcernError" << kMockWriteConcernError);

const Status kRetryableError{ErrorCodes::HostUnreachable, "RetryableError for test"};

class ShardingRefresherTest : public ConfigServerTestFixture {
protected:
    void setUp() override {
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

TEST_F(ShardingRefresherTest, refresherTwoShardsSucceed) {
    auto opCtx = operationContext();
    auto nss = NamespaceString::createNamespaceString_forTest("mydb", "mycoll");
    auto future = launchAsync([&] {
        sharding_util::tellShardsToRefreshCollection(opCtx, kShardIdList, nss, executor());
    });

    onCommand([&](const executor::RemoteCommandRequest& request) { return BSON("ok" << 1); });
    onCommand([&](const executor::RemoteCommandRequest& request) { return BSON("ok" << 1); });

    future.default_timed_get();
}

TEST_F(ShardingRefresherTest, refresherTwoShardsFirstErrors) {
    auto opCtx = operationContext();
    auto nss = NamespaceString::createNamespaceString_forTest("mydb", "mycoll");
    auto future = launchAsync([&] {
        sharding_util::tellShardsToRefreshCollection(opCtx, kShardIdList, nss, executor());
    });

    onCommand([&](const executor::RemoteCommandRequest& request) { return kMockErrorRes; });

    ASSERT_THROWS_CODE(future.default_timed_get(), DBException, kMockStatus.code());
}

TEST_F(ShardingRefresherTest, refresherTwoShardsSecondErrors) {
    auto opCtx = operationContext();
    auto nss = NamespaceString::createNamespaceString_forTest("mydb", "mycoll");
    auto future = launchAsync([&] {
        sharding_util::tellShardsToRefreshCollection(opCtx, kShardIdList, nss, executor());
    });

    onCommand([&](const executor::RemoteCommandRequest& request) { return BSON("ok" << 1); });
    onCommand([&](const executor::RemoteCommandRequest& request) { return kMockErrorRes; });

    ASSERT_THROWS_CODE(future.default_timed_get(), DBException, kMockStatus.code());
}

TEST_F(ShardingRefresherTest, refresherTwoShardsWriteConcernTimeout) {
    auto opCtx = operationContext();
    auto nss = NamespaceString::createNamespaceString_forTest("mydb", "mycoll");
    auto future = launchAsync([&] {
        sharding_util::tellShardsToRefreshCollection(opCtx, kShardIdList, nss, executor());
    });

    onCommand([&](const executor::RemoteCommandRequest& request) {
        return kMockResWithWriteConcernError;
    });

    ASSERT_THROWS_CODE(future.default_timed_get(), DBException, ErrorCodes::WriteConcernTimeout);
}

}  // namespace
}  // namespace mongo
