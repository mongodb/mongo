// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/base/counter.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/write_concern.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/timer.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <utility>
#include <vector>

#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

class SimpleClient {
public:
    SimpleClient(OperationContext* opCtx) : _client(opCtx), _opCtx(opCtx) {}

    Status insert(const NamespaceString& nss, const BSONObj& doc) {
        const auto commandResponse = _client.runCommand([&] {
            write_ops::InsertCommandRequest insertOp(nss);
            insertOp.setDocuments({doc});
            return insertOp.serialize();
        }());

        auto status = getStatusFromWriteCommandReply(commandResponse->getCommandReply());

        WriteConcernResult ignoreResult;
        auto latestOpTime = repl::ReplClientInfo::forClient(_opCtx->getClient()).getLastOp();
        uassertStatusOK(waitForWriteConcern(
            _opCtx, latestOpTime, defaultMajorityWriteConcernDoNotUse(), &ignoreResult));

        return status;
    }

private:
    DBDirectClient _client;
    OperationContext* _opCtx;
};

using ReshardingCollectionTest = ShardServerTestFixture;

TEST_F(ReshardingCollectionTest, TestWritesToTempReshardingCollection) {
    SimpleClient client(operationContext());
    auto uuid = UUID::gen();

    auto tempNss = NamespaceString::createNamespaceString_forTest(
        fmt::format("{}.system.resharding.{}", "test", uuid.toString()));
    ASSERT_OK(client.insert(tempNss, BSON("x" << 5)));

    auto chunksNss = NamespaceString::createNamespaceString_forTest(
        fmt::format("config.cache.chunks.{}.system.resharding.{}", "test", uuid.toString()));
    ASSERT_OK(client.insert(chunksNss, BSON("X" << 5)));
}

// TODO(SERVER-59325): Remove stress test when no longer needed.
TEST_F(ReshardingCollectionTest, TestWritesToTempReshardingCollectionStressTest) {
    static constexpr int kThreads = 10;
    std::vector<stdx::thread> threads;
    Counter64 iterations;

    for (int t = 0; t < kThreads; ++t) {
        stdx::thread thread([&]() {
            Timer timer;
            while (timer.elapsed() < Seconds(2)) {
                ThreadClient threadClient(getGlobalServiceContext()->getService());
                auto opCtx = Client::getCurrent()->makeOperationContext();
                invariant(opCtx);
                SimpleClient client(opCtx.get());
                auto uuid = UUID::gen();

                auto tempNss = NamespaceString::createNamespaceString_forTest(
                    fmt::format("{}.system.resharding.{}", "test", uuid.toString()));
                ASSERT_OK(client.insert(tempNss, BSON("x" << 5)));

                auto chunksNss = NamespaceString::createNamespaceString_forTest(fmt::format(
                    "config.cache.chunks.{}.system.resharding.{}", "test", uuid.toString()));
                ASSERT_OK(client.insert(chunksNss, BSON("X" << 5)));
                iterations.increment();
            }
        });
        threads.push_back(std::move(thread));
    }
    for (auto& t : threads) {
        t.join();
    }

    LOGV2(5930701, "Stress test completed", "iterations"_attr = iterations.get());
}

}  // namespace
}  // namespace mongo
