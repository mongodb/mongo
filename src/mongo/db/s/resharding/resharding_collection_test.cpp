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


#include "mongo/base/counter.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
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
