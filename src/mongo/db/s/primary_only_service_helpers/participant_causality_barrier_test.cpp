// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

        _executor = executor::ThreadPoolTaskExecutor::create(
            ThreadPool::make({
                .poolName = "ParticipantCausalityBarrierTest",
            }),
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
