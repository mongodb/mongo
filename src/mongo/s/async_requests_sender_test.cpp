// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include <boost/move/utility_core.hpp>
// IWYU pragma: no_include "cxxabi.h"
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/client/retry_strategy_server_parameters_gen.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/sharding_environment/shard_handle.h"
#include "mongo/db/sharding_environment/shard_ref.h"
#include "mongo/db/sharding_environment/shard_shared_state_cache.h"
#include "mongo/db/sharding_environment/sharding_mongos_test_fixture.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/otel/traces/span/span.h"
#include "mongo/otel/traces/span/span_names.h"
#include "mongo/otel/traces/traces_test_util.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <functional>
#include <system_error>

#include <absl/container/flat_hash_map.h>

namespace mongo {

namespace {
using otel::traces::HasSpanName;
using otel::traces::Parent;
using ::testing::ElementsAre;

using namespace std::literals::string_view_literals;

const NamespaceString kTestNss = NamespaceString::createNamespaceString_forTest("testdb.testcoll");
const HostAndPort kTestConfigShardHost = HostAndPort("FakeConfigHost", 12345);
const std::vector<ShardHandle> kTestShardHandles = {
    ShardHandle(ShardId("FakeShard1"), boost::make_optional(UUID::gen())),
    ShardHandle(ShardId("FakeShard2"), boost::make_optional(UUID::gen())),
    ShardHandle(ShardId("FakeShard3"), boost::make_optional(UUID::gen())),
};

// Here we have an array of vector containing hosts as we test the ARS with a replica set
// connection string. This allows us to verify whether ARS applies targeting changes when a server
// is detected to be overloaded.
const std::array kTestShardHosts = {
    std::vector<HostAndPort>{HostAndPort("FakeShardRS1Host1", 12345),
                             HostAndPort("FakeShardRS1Host2", 12345)},
    std::vector<HostAndPort>{HostAndPort("FakeShardRS2Host1", 12345),
                             HostAndPort("FakeShardRS2Host2", 12345)},
    std::vector<HostAndPort>{HostAndPort("FakeShardRS3Host1", 12345),
                             HostAndPort("FakeShardRS3Host2", 12345)},
};

enum class ShardRefKind { kName, kUuid };
class AsyncRequestsSenderTest : public ShardingTestFixture,
                                public testing::WithParamInterface<ShardRefKind> {
public:
    AsyncRequestsSenderTest() {}

    void setUp() override {
        ShardingTestFixture::setUp();

        configTargeter()->setFindHostReturnValue(kTestConfigShardHost);

        std::vector<ShardType> shards;

        for (size_t i = 0; i < kTestShardHandles.size(); i++) {
            ShardType shardType;
            auto host = ConnectionString::forReplicaSet(kTestShardHandles[i].name().toString(),
                                                        kTestShardHosts[i]);
            shardType.setHandle(kTestShardHandles[i]);
            shardType.setHost(host.toString());

            shards.push_back(shardType);

            std::unique_ptr<RemoteCommandTargeterMock> targeter(
                std::make_unique<RemoteCommandTargeterMock>());
            _targeters.push_back(targeter.get());

            targeter->setConnectionStringReturnValue(host);
            targeter->setFindHostsReturnValue(kTestShardHosts[i]);

            targeterFactory()->addTargeterToReturn(host, std::move(targeter));
        }

        setupShards(shards);
    }

protected:
    ShardRef shardRef(const ShardHandle& handle) const {
        switch (GetParam()) {
            case ShardRefKind::kName:
                return ShardRef(handle.name());
            case ShardRefKind::kUuid:
                return ShardRef(*handle.uuid());
        }
        MONGO_UNREACHABLE;
    }

    ShardRef shardRef(size_t index) const {
        return shardRef(kTestShardHandles[index]);
    }

    static constexpr int kMaxCommandExecutions = kDefaultClientMaxRetryAttemptsDefault + 1;

    std::vector<RemoteCommandTargeterMock*> _targeters;  // Targeters are owned by the factory.
};

INSTANTIATE_TEST_SUITE_P(ShardRefKind,
                         AsyncRequestsSenderTest,
                         testing::Values(ShardRefKind::kName, ShardRefKind::kUuid),
                         [](const testing::TestParamInfo<ShardRefKind>& info) {
                             return info.param == ShardRefKind::kName ? "ByName" : "ByUuid";
                         });

TEST_P(AsyncRequestsSenderTest, HandlesExceptionWhenYielding) {
    class ThrowyResourceYielder : public ResourceYielder {
    public:
        void yield(OperationContext*) override {
            if (_count++) {
                uasserted(ErrorCodes::BadValue, "Simulated error");
            }
        }

        void unyield(OperationContext*) override {}

    private:
        int _count = 0;
    };

    std::vector<AsyncRequestsSender::Request> requests;
    requests.emplace_back(shardRef(0), BSON("find" << "bar"));
    requests.emplace_back(shardRef(1), BSON("find" << "bar"));
    requests.emplace_back(shardRef(2), BSON("find" << "bar"));

    auto ars = AsyncRequestsSender(operationContext(),
                                   executor(),
                                   kTestNss.dbName(),
                                   requests,
                                   ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                   Shard::RetryPolicy::kNoRetry,
                                   std::make_unique<ThrowyResourceYielder>(),
                                   {} /* designatedHostsMap */);

    // Issue blocking waits on a different thread.
    auto future = launchAsync([&]() {
        // Yield doesn't throw the first time.
        auto response = ars.next();
        ASSERT(response.swResponse.getStatus().isOK());
        ASSERT_EQ(response.shardId, shardRef(0));

        // Yield throws here and all outstanding responses, including the one currently being waited
        // on, are cancelled with the error yield threw.
        response = ars.next();
        ASSERT_EQ(response.swResponse.getStatus(), ErrorCodes::BadValue);
        ASSERT_EQ(response.shardId, shardRef(1));

        response = ars.next();
        ASSERT_EQ(response.swResponse.getStatus(), ErrorCodes::BadValue);
        ASSERT_EQ(response.shardId, shardRef(2));
    });

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        return CursorResponse(kTestNss, 0LL, {BSON("x" << 1)})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    future.default_timed_get();
}

TEST_P(AsyncRequestsSenderTest, HandlesExceptionWhenUnyielding) {
    class ThrowyResourceYielder : public ResourceYielder {
    public:
        void yield(OperationContext*) override {}

        void unyield(OperationContext*) override {
            if (_count++ == 1) {
                uasserted(ErrorCodes::BadValue, "Simulated error");
            }
        }

    private:
        int _count = 0;
    };

    std::vector<AsyncRequestsSender::Request> requests;
    requests.emplace_back(shardRef(0), BSON("find" << "bar"));
    requests.emplace_back(shardRef(1), BSON("find" << "bar"));
    requests.emplace_back(shardRef(2), BSON("find" << "bar"));
    std::set<ShardRef> pendingShardRefs{shardRef(0), shardRef(1), shardRef(2)};

    auto ars = AsyncRequestsSender(operationContext(),
                                   executor(),
                                   kTestNss.dbName(),
                                   requests,
                                   ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                   Shard::RetryPolicy::kNoRetry,
                                   std::make_unique<ThrowyResourceYielder>(),
                                   {} /* designatedHostsMap */);

    auto firstResponseProcessed = unittest::Barrier(2);

    // Issue blocking waits on a different thread.
    auto future = launchAsync([&]() {
        // Unyield doesn't throw the first time.
        auto response = ars.next();
        ASSERT(response.swResponse.getStatus().isOK());
        pendingShardRefs.erase(response.shardId);

        firstResponseProcessed.countDownAndWait();

        // Unyield throws this time. Even if the next response was already ready and successful,
        // the returned response should have the unyield error.
        response = ars.next();
        ASSERT_EQ(response.swResponse.getStatus(), ErrorCodes::BadValue);
        pendingShardRefs.erase(response.shardId);

        // Unyield doesn't throw this time but this next() call should not even try to yield and
        // unyield and the returned response should have the unyield error.
        response = ars.next();
        ASSERT_EQ(response.swResponse.getStatus(), ErrorCodes::BadValue);
        pendingShardRefs.erase(response.shardId);
    });

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        return CursorResponse(kTestNss, 0LL, {BSON("x" << 1)})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    firstResponseProcessed.countDownAndWait();

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        return CursorResponse(kTestNss, 0LL, {BSON("x" << 1)})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    future.default_timed_get();
    ASSERT(pendingShardRefs.empty());
}

TEST_P(AsyncRequestsSenderTest, ExceptionWhileWaitingDoesNotSkipUnyield) {
    class CountingResourceYielder : public ResourceYielder {
    public:
        void yield(OperationContext*) override {
            ++timesYielded;
        }

        void unyield(OperationContext*) override {
            ++timesUnyielded;
        }

        int timesYielded = 0;
        int timesUnyielded = 0;
    };

    std::vector<AsyncRequestsSender::Request> requests;
    requests.emplace_back(shardRef(0), BSON("find" << "bar"));

    auto yielder = std::make_unique<CountingResourceYielder>();
    auto yielderPointer = yielder.get();
    auto ars = AsyncRequestsSender(operationContext(),
                                   executor(),
                                   kTestNss.dbName(),
                                   requests,
                                   ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                   Shard::RetryPolicy::kNoRetry,
                                   std::move(yielder),
                                   {} /* designatedHostsMap */);

    // Issue blocking wait on a different thread.
    auto future = launchAsync([&]() {
        // Unyield doesn't throw the first time.
        auto response = ars.next();
        ASSERT_EQ(response.swResponse.getStatus(), ErrorCodes::Interrupted);
        ASSERT_EQ(response.shardId, shardRef(0));
    });

    // Interrupt the waiting opCtx and verify unyield wasn't called.
    operationContext()->markKilled();

    future.default_timed_get();

    ASSERT_EQ(yielderPointer->timesYielded, 1);
    ASSERT_EQ(yielderPointer->timesUnyielded, 1);
}

TEST_P(AsyncRequestsSenderTest, DesignatedHostChosen) {
    std::vector<AsyncRequestsSender::Request> requests;
    requests.emplace_back(shardRef(0), BSON("find" << "bar"));
    requests.emplace_back(shardRef(1), BSON("find" << "bar"));
    requests.emplace_back(shardRef(2), BSON("find" << "bar"));

    AsyncRequestsSender::ShardHostMap designatedHosts;

    auto shard1Secondary = kTestShardHosts[1][1];
    _targeters[1]->setConnectionStringReturnValue(
        ConnectionString::forReplicaSet("shard1_rs"sv, kTestShardHosts[1]));
    designatedHosts[shardRef(1)] = shard1Secondary;
    auto ars = AsyncRequestsSender(operationContext(),
                                   executor(),
                                   kTestNss.dbName(),
                                   requests,
                                   ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                   Shard::RetryPolicy::kNoRetry,
                                   nullptr /* no yielder */,
                                   designatedHosts);

    auto future = launchAsync([&]() {
        auto response = ars.next();
        ASSERT(response.swResponse.getStatus().isOK());
        ASSERT_EQ(response.shardId, shardRef(0));
        ASSERT_EQ(response.shardHostAndPort, kTestShardHosts[0].front());

        response = ars.next();
        ASSERT(response.swResponse.getStatus().isOK());
        ASSERT_EQ(response.shardId, shardRef(1));
        ASSERT_EQ(response.shardHostAndPort, shard1Secondary);

        response = ars.next();
        ASSERT(response.swResponse.getStatus().isOK());
        ASSERT_EQ(response.shardId, shardRef(2));
        ASSERT_EQ(response.shardHostAndPort, kTestShardHosts[2].front());
    });

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        ASSERT_EQ(request.target, kTestShardHosts[0].front());
        return CursorResponse(kTestNss, 0LL, {BSON("x" << 1)})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        ASSERT_EQ(request.target, shard1Secondary);
        return CursorResponse(kTestNss, 0LL, {BSON("x" << 2)})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        ASSERT_EQ(request.target, kTestShardHosts[2].front());
        return CursorResponse(kTestNss, 0LL, {BSON("x" << 3)})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });
    future.default_timed_get();
}

TEST_P(AsyncRequestsSenderTest, DesignatedHostMustBeInShard) {
    std::vector<AsyncRequestsSender::Request> requests;
    requests.emplace_back(shardRef(0), BSON("find" << "bar"));
    requests.emplace_back(shardRef(1), BSON("find" << "bar"));
    requests.emplace_back(shardRef(2), BSON("find" << "bar"));

    AsyncRequestsSender::ShardHostMap designatedHosts;
    designatedHosts[shardRef(1)] = HostAndPort("HostNotInShard", 12345);
    auto ars = AsyncRequestsSender(operationContext(),
                                   executor(),
                                   kTestNss.dbName(),
                                   requests,
                                   ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                   Shard::RetryPolicy::kNoRetry,
                                   nullptr /* no yielder */,
                                   designatedHosts);

    // We see the error immediately, because it happens in construction.
    auto response = ars.next();
    ASSERT_EQ(response.swResponse.getStatus(), ErrorCodes::HostNotFound);
    ASSERT_EQ(response.shardId, shardRef(1));
}

TEST_P(AsyncRequestsSenderTest, PreLoadedShardIsUsedForInitialRequest) {
    auto shardRegistry = Grid::get(operationContext())->shardRegistry();
    auto shard0 =
        uassertStatusOK(shardRegistry->getShard(operationContext(), kTestShardHandles[0].name()));
    auto shard1 =
        uassertStatusOK(shardRegistry->getShard(operationContext(), kTestShardHandles[1].name()));
    auto shard2 =
        uassertStatusOK(shardRegistry->getShard(operationContext(), kTestShardHandles[2].name()));

    // Intentionally provide ShardRefs mismatched with Shard types to prove the Shard given in the
    // request is used for the initial attempt.
    std::vector<AsyncRequestsSender::Request> requests;
    requests.emplace_back(shardRef(0), BSON("find" << "bar"), shard1);
    requests.emplace_back(shardRef(1), BSON("find" << "bar"), shard2);
    requests.emplace_back(shardRef(2), BSON("find" << "bar"), shard0);

    auto ars = AsyncRequestsSender(operationContext(),
                                   executor(),
                                   kTestNss.dbName(),
                                   requests,
                                   ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                   Shard::RetryPolicy::kIdempotent,
                                   nullptr /* no yielder */,
                                   {} /* designatedHostsMap */);

    auto future = launchAsync([&]() {
        auto response = ars.next();
        ASSERT(response.swResponse.getStatus().isOK());
        ASSERT_EQ(response.shardId, shardRef(0));
        ASSERT_EQ(response.shardHostAndPort, kTestShardHosts[1].front());

        response = ars.next();
        ASSERT(response.swResponse.getStatus().isOK());
        ASSERT_EQ(response.shardId, shardRef(1));
        ASSERT_EQ(response.shardHostAndPort, kTestShardHosts[2].front());

        response = ars.next();
        ASSERT(response.swResponse.getStatus().isOK());
        ASSERT_EQ(response.shardId, shardRef(2));
        // The ARS initially targets the host in Shard0 because that is the provided Shard but it
        // retries on a network error and "refreshes," targeting the host in Shard2.
        ASSERT_EQ(response.shardHostAndPort, kTestShardHosts[2].front());
    });

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        ASSERT_EQ(request.target, kTestShardHosts[1].front());
        return CursorResponse(kTestNss, 0LL, {BSON("x" << 1)})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        ASSERT_EQ(request.target, kTestShardHosts[2].front());
        return CursorResponse(kTestNss, 0LL, {BSON("x" << 2)})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    // The initial attempt targets the host in shard0 because that was the provided shard. Return a
    // retriable error to verify the provided shard is only used for the initial attempt.
    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        ASSERT_EQ(request.target, kTestShardHosts[0].front());
        return Status(ErrorCodes::HostUnreachable, "mock network error");
    });

    // Retry targets the "right" host in Shard2.
    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        ASSERT_EQ(request.target, kTestShardHosts[2].front());
        return Status(ErrorCodes::HostUnreachable, "mock network error");
    });

    // Further retries also use the reloaded Shard.
    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        ASSERT_EQ(request.target, kTestShardHosts[2].front());
        return CursorResponse(kTestNss, 0LL, {BSON("x" << 3)})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    future.default_timed_get();
}

TEST_P(AsyncRequestsSenderTest, MultipleRetriesReceivedInconclusiveError) {
    std::vector<AsyncRequestsSender::Request> requests;
    requests.emplace_back(shardRef(0), BSON("find" << "bar"));
    requests.emplace_back(shardRef(1), BSON("find" << "bar"));
    requests.emplace_back(shardRef(2), BSON("find" << "bar"));

    const BSONObj writeConcernError = BSON("code" << ErrorCodes::HostUnreachable << "errmsg"
                                                  << "Third mock network error");
    BSONObj resWithWriteConcernError = BSON("ok" << 1 << "writeConcernError" << writeConcernError);

    auto ars = AsyncRequestsSender(operationContext(),
                                   executor(),
                                   kTestNss.dbName(),
                                   requests,
                                   ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                   Shard::RetryPolicy::kIdempotent,
                                   nullptr,
                                   {});

    auto future = launchAsync([&]() {
        auto response = ars.next();
        ASSERT(response.swResponse.getStatus().isOK());

        response = ars.next();
        ASSERT(response.swResponse.getStatus().isOK());

        response = ars.next();
        ASSERT(response.swResponse.getStatus().isOK());
        ASSERT_BSONOBJ_EQ(response.swResponse.getValue().data, resWithWriteConcernError);
    });

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        return CursorResponse(kTestNss, 0LL, {BSON("x" << 1)})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        return CursorResponse(kTestNss, 0LL, {BSON("x" << 2)})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        return Status(ErrorCodes::HostUnreachable, "Mock network error");
    });

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        BSONObj res = BSON("ok" << 1 << "writeConcernError"
                                << BSON("code" << ErrorCodes::HostUnreachable << "errmsg"
                                               << "Second mock network error"));
        return res;
    });

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        return resWithWriteConcernError;
    });

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        return Status(ErrorCodes::NotWritablePrimary, "NotWritablePrimary error");
    });

    future.default_timed_get();
}

TEST_P(AsyncRequestsSenderTest, MultipleRetriesSystemOverloaded) {
    std::vector<AsyncRequestsSender::Request> requests;
    requests.emplace_back(shardRef(0), BSON("find" << "bar"));
    requests.emplace_back(shardRef(1), BSON("find" << "bar"));
    requests.emplace_back(shardRef(2), BSON("find" << "bar"));

    constexpr Milliseconds baseBackoffMS{500};

    FailPointEnableBlock fp{"returnMaxBackoffDelay"};

    auto shardState =
        ShardSharedStateCache::get(operationContext()).getShardState(kTestShardHandles[2].name());

    BSONObj resWithSystemOverloadedError =
        createErrorSystemOverloaded(ErrorCodes::IngressRequestRateLimitExceeded, baseBackoffMS);

    auto ars = AsyncRequestsSender(operationContext(),
                                   executor(),
                                   kTestNss.dbName(),
                                   requests,
                                   ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                   Shard::RetryPolicy::kIdempotent,
                                   nullptr,
                                   {});

    auto future = launchAsync([&]() {
        auto response = ars.next();
        ASSERT(response.swResponse.getStatus().isOK());

        response = ars.next();
        ASSERT(response.swResponse.getStatus().isOK());

        response = ars.next();
        ASSERT(response.swResponse.getStatus().isOK());
        auto errorLabels = response.swResponse.getValue().getErrorLabels();
        ASSERT(std::ranges::find(errorLabels, ErrorLabel::kSystemOverloadedError) !=
               errorLabels.end());
        ASSERT_BSONOBJ_EQ(response.swResponse.getValue().data, resWithSystemOverloadedError);
    });

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        return CursorResponse(kTestNss, 0LL, {BSON("x" << 1)})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        return CursorResponse(kTestNss, 0LL, {BSON("x" << 2)})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    for (int i = 0; i < kMaxCommandExecutions; ++i) {
        onCommand([&](const auto& request) {
            ASSERT(request.cmdObj["find"]);
            return resWithSystemOverloadedError;
        });

        if (i < kDefaultClientMaxRetryAttemptsDefault) {
            advanceUntilReadyRequest();
        }
    }

    constexpr auto kExpectedTotalBackoffWithBaseBackoffMS = Milliseconds{1000 + 2000 + 4000};
    ASSERT_EQ(shardState->stats.totalBackoffTimeMillis.load(),
              kExpectedTotalBackoffWithBaseBackoffMS.count());

    future.default_timed_get();
}

TEST_P(AsyncRequestsSenderTest, DifferentTelemetryContextsSentPerShard) {
    otel::traces::OtelTracesCapturer capturer;
    if (!otel::traces::OtelTracesCapturer::canReadSpans()) {
        return;
    }

    std::vector<otel::traces::SpanName> childSpanNames = {
        otel::traces::span_names::kTest2,
        otel::traces::span_names::kTest3,
        otel::traces::span_names::kTest4,
    };
    // Maps each shard's target host to the telemetry context that was sent along with its
    // request, so we can verify every shard got its own distinct context.
    absl::flat_hash_map<HostAndPort, otel::TelemetryContext*> telemetryContextsByTarget;
    {  // Start a real span on the opCtx so that RemoteData's cloneTelemetryContext has an active
        // trace to clone; otherwise, no telemetry context would be created at all. We will end the
        // span so that we can test span parenthood.
        auto span = otel::traces::Span::start(operationContext(), otel::traces::span_names::kTest1);

        std::vector<AsyncRequestsSender::Request> requests;
        requests.emplace_back(shardRef(0), BSON("find" << "bar"));
        requests.emplace_back(shardRef(1), BSON("find" << "bar"));
        requests.emplace_back(shardRef(2), BSON("find" << "bar"));

        auto ars = AsyncRequestsSender(operationContext(),
                                       executor(),
                                       kTestNss.dbName(),
                                       requests,
                                       ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                       Shard::RetryPolicy::kNoRetry,
                                       nullptr /* no yielder */,
                                       {} /* designatedHostsMap */);

        auto future = launchAsync([&]() {
            for (int i = 0; i < 3; ++i) {
                auto response = ars.next();
                ASSERT(response.swResponse.getStatus().isOK());
            }
        });

        for (int i = 0; i < 3; ++i) {
            onCommand([this, &telemetryContextsByTarget, &childSpanNames, i](const auto& request) {
                ASSERT(request.cmdObj["find"]);
                ASSERT_TRUE(static_cast<bool>(request.telemetryContext));
                EXPECT_TRUE(request.telemetryContext->hasActiveTrace());
                telemetryContextsByTarget[request.target] = request.telemetryContext.get();
                // Start a span so we can verify the parent is correct.
                auto span = otel::traces::Span::start(operationContext(), childSpanNames[i]);
                return CursorResponse(kTestNss, 0LL, {BSON("x" << 1)})
                    .toBSON(CursorResponse::ResponseType::InitialResponse);
            });
        }

        future.default_timed_get();
    }

    ASSERT_EQ(telemetryContextsByTarget.size(), 3);
    EXPECT_NE(telemetryContextsByTarget[kTestShardHosts[0].front()],
              telemetryContextsByTarget[kTestShardHosts[1].front()]);
    EXPECT_NE(telemetryContextsByTarget[kTestShardHosts[1].front()],
              telemetryContextsByTarget[kTestShardHosts[2].front()]);
    EXPECT_NE(telemetryContextsByTarget[kTestShardHosts[0].front()],
              telemetryContextsByTarget[kTestShardHosts[2].front()]);

    EXPECT_THAT(capturer.getSpans(childSpanNames[0]),
                ElementsAre(Parent(HasSpanName(otel::traces::span_names::kTest1))));
    EXPECT_THAT(capturer.getSpans(childSpanNames[1]),
                ElementsAre(Parent(HasSpanName(otel::traces::span_names::kTest1))));
    EXPECT_THAT(capturer.getSpans(childSpanNames[2]),
                ElementsAre(Parent(HasSpanName(otel::traces::span_names::kTest1))));
}

TEST_P(AsyncRequestsSenderTest, SameTelemetryContextAcrossRetriesForSameShard) {
    otel::traces::OtelTracesCapturer capturer;
    if (!otel::traces::OtelTracesCapturer::canReadSpans()) {
        return;
    }

    auto span = otel::traces::Span::start(operationContext(), otel::traces::span_names::kTest1);

    std::vector<AsyncRequestsSender::Request> requests;
    requests.emplace_back(shardRef(0), BSON("find" << "bar"));

    auto ars = AsyncRequestsSender(operationContext(),
                                   executor(),
                                   kTestNss.dbName(),
                                   requests,
                                   ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                   Shard::RetryPolicy::kIdempotent,
                                   nullptr /* no yielder */,
                                   {} /* designatedHostsMap */);

    std::shared_ptr<otel::TelemetryContext> firstAttemptCtx;
    std::shared_ptr<otel::TelemetryContext> secondAttemptCtx;

    auto future = launchAsync([&]() {
        auto response = ars.next();
        ASSERT(response.swResponse.getStatus().isOK());
    });

    // The first attempt fails with a retriable error; the retry should reuse the same telemetry
    // context rather than creating a new one, since both attempts belong to the same RemoteData.
    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        ASSERT_TRUE(static_cast<bool>(request.telemetryContext));
        firstAttemptCtx = request.telemetryContext;
        return Status(ErrorCodes::HostUnreachable, "mock network error");
    });

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        ASSERT_TRUE(static_cast<bool>(request.telemetryContext));
        secondAttemptCtx = request.telemetryContext;
        return CursorResponse(kTestNss, 0LL, {BSON("x" << 1)})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    future.default_timed_get();

    EXPECT_TRUE(static_cast<bool>(firstAttemptCtx));
    EXPECT_TRUE(static_cast<bool>(secondAttemptCtx));
    EXPECT_EQ(firstAttemptCtx, secondAttemptCtx);
}

}  // namespace
}  // namespace mongo
