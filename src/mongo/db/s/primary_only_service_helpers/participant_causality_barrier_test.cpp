/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/s/primary_only_service_helpers/participant_causality_barrier.h"

#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/executor/mock_async_rpc.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

bool requestOsiMatches(const write_ops::UpdateCommandRequest& cmd,
                       const OperationSessionInfo& expected) {
    const auto& args = cmd.getGenericArguments();
    return args.getLsid() && expected.getSessionId() &&
        args.getLsid()->getId() == expected.getSessionId()->getId() &&
        args.getTxnNumber() == expected.getTxnNumber();
}

class ParticipantCausalityBarrierTest : public service_context_test::WithSetupTransportLayer,
                                        public ShardServerTestFixture {
public:
    void setUp() override {
        ShardServerTestFixture::setUp();
        addRemoteShards({{kShardId, HostAndPort("shard0", 1234)}});

        ThreadPool::Options threadPoolOptions;
        threadPoolOptions.poolName = "ParticipantCausalityBarrierTest";
        _executor = executor::ThreadPoolTaskExecutor::create(
            std::make_unique<ThreadPool>(threadPoolOptions),
            executor::makeNetworkInterface("ParticipantCausalityBarrierTest"));
        _executor->startup();

        auto mock = std::make_unique<async_rpc::AsyncMockAsyncRPCRunner>();
        async_rpc::detail::AsyncRPCRunner::set(getServiceContext(), std::move(mock));
    }

    void tearDown() override {
        _executor->shutdown();
        ShardServerTestFixture::tearDown();
    }

protected:
    const ShardId kShardId{"shard0"};
    std::shared_ptr<executor::ThreadPoolTaskExecutor> _executor;
};

TEST_F(ParticipantCausalityBarrierTest, NoopWriteRequestIncludesProvidedOSI) {
    OperationSessionInfo osi;
    osi.setSessionId(makeLogicalSessionIdForTest());
    osi.setTxnNumber(TxnNumber{42});

    auto* runner = dynamic_cast<async_rpc::AsyncMockAsyncRPCRunner*>(
        async_rpc::detail::AsyncRPCRunner::get(operationContext()->getServiceContext()));

    auto expectation = runner->expect(
        [&osi](const auto& req) {
            auto cmd = write_ops::UpdateCommandRequest::parse(
                req.cmdBSON.addFields(BSON("$db" << req.dbName)),
                IDLParserContext("ParticipantCausalityBarrierTest"));
            return requestOsiMatches(cmd, osi);
        },
        BSON("ok" << 1),
        "NoopWriteRequestIncludesProvidedOSI");

    ParticipantCausalityBarrier barrier{
        {kShardId}, _executor, operationContext()->getCancellationToken()};
    barrier.perform(operationContext(), osi);

    expectation.get();
}

}  // namespace
}  // namespace mongo
