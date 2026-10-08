// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/router_role/cluster_commands_helpers.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands.h"
#include "mongo/db/global_catalog/ddl/cannot_implicitly_create_collection_info.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/sharding_mongos_test_fixture.h"
#include "mongo/db/versioning_protocol/stale_exception.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/session_catalog_router.h"
#include "mongo/s/transaction_router.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/hostandport.h"

#include <tuple>
#include <vector>

namespace mongo {
namespace {

const NamespaceString kTestNss = NamespaceString::createNamespaceString_forTest("test.bar");
const ShardId kTestRemoteShardId1 = ShardId("FakeShard1");
const HostAndPort kTestRemoteHostAndPort1 = HostAndPort("FakeShard1Host", 12345);
const ShardId kTestRemoteShardId2 = ShardId("FakeShard2");
const HostAndPort kTestRemoteHostAndPort2 = HostAndPort("FakeShard2Host", 12345);
const ShardId kTestRemoteShardId3 = ShardId("FakeShard3");

class GatherResponsesTest : public virtual service_context_test::RouterRoleOverride,
                            public ShardingTestFixture {
public:
    void setUp() override {
        ShardingTestFixture::setUp();

        configTargeter()->setFindHostReturnValue(HostAndPort("FakeConfigHost", 12345));

        // addRemoteShards() re-mocks the config.shards find to return exactly the shards passed,
        // so both shards must be registered in one call; a second call would evict the first.
        std::vector<std::tuple<ShardId, HostAndPort>> shardInfos;
        shardInfos.emplace_back(std::make_tuple(kTestRemoteShardId1, kTestRemoteHostAndPort1));
        shardInfos.emplace_back(std::make_tuple(kTestRemoteShardId2, kTestRemoteHostAndPort2));
        addRemoteShards(shardInfos);
    }

    void tearDown() override {
        _routerOperationContextSession.reset();
        ShardingTestFixture::tearDown();
    }

protected:
    void beginTransaction() {
        operationContext()->setLogicalSessionId(makeLogicalSessionIdForTest());
        operationContext()->setTxnNumber(TxnNumber(0));
        operationContext()->setInMultiDocumentTransaction();
        _routerOperationContextSession.emplace(operationContext());
        auto txnRouter = TransactionRouter::get(operationContext());
        txnRouter.beginOrContinueTxn(
            operationContext(), TxnNumber(0), TransactionRouter::TransactionActions::kStart);
        txnRouter.setDefaultAtClusterTime(operationContext());
    }

    std::vector<AsyncRequestsSender::Response> gatherTwoShards(
        Shard::RetryPolicy retryPolicy = Shard::RetryPolicy::kNoRetry) {
        std::vector<AsyncRequestsSender::Request> requests{
            {kTestRemoteShardId1, BSON("find" << "bar")},
            {kTestRemoteShardId2, BSON("find" << "bar")}};
        return gatherResponses(operationContext(),
                               kTestNss.dbName(),
                               kTestNss,
                               ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                               retryPolicy,
                               requests,
                               nullptr /* routingCtx */,
                               executor());
    }

    BSONObj errorResponse(Status status) {
        BSONObjBuilder res;
        CommandHelpers::appendCommandStatusNoThrow(res, status);
        return res.obj();
    }

    Status staleDbVersionStatus() {
        return {StaleDbRoutingVersion(kTestNss.dbName(), DatabaseVersion::makeFixed(), boost::none),
                "stale db version"};
    }

    BSONObj okWithAdditionalParticipant(ShardId shardId, long long term) {
        return BSON(
            "ok" << 1 << "readOnly" << true << "additionalParticipants"
                 << BSON_ARRAY(BSON("shardId" << shardId << "readOnly" << true << "term" << term)));
    }

private:
    boost::optional<RouterOperationContextSession> _routerOperationContextSession;
};

TEST_F(GatherResponsesTest, ReturnsAllResponsesWhenNoResponseBailsOut) {
    beginTransaction();

    std::vector<AsyncRequestsSender::Response> responses;
    auto future = launchAsync([&]() { responses = gatherTwoShards(); });

    onCommand([&](const auto& request) {
        ASSERT_EQ(request.target, kTestRemoteHostAndPort1);
        return BSON("ok" << 1 << "readOnly" << true);
    });
    onCommand([&](const auto& request) {
        ASSERT_EQ(request.target, kTestRemoteHostAndPort2);
        return BSON("ok" << 1 << "readOnly" << true);
    });

    future.default_timed_get();
    ASSERT_EQ(responses.size(), 2);
}

// A bail-out error (here StaleDbVersion) must not abandon the sibling response still in flight:
// the loop drains it — enrolling its announced participant — and only then throws.
TEST_F(GatherResponsesTest, DrainsSiblingResponsesAfterBailOut) {
    beginTransaction();
    auto txnRouter = TransactionRouter::get(operationContext());

    auto future = launchAsync([&]() {
        ASSERT_THROWS_CODE(gatherTwoShards(), AssertionException, ErrorCodes::StaleDbVersion);
    });

    {
        // Deliver the second response only after next() has consumed the first, so the
        // arrival order is deterministic.
        FailPointEnableBlock fp("hangBeforePollResponse", BSON("remotesLeft" << 1));
        onCommand([&](const auto& request) {
            ASSERT_EQ(request.target, kTestRemoteHostAndPort1);
            return errorResponse(staleDbVersionStatus());
        });
        fp->waitForTimesEntered(fp.initialTimesEntered() + 1);
    }
    onCommand([&](const auto& request) {
        ASSERT_EQ(request.target, kTestRemoteHostAndPort2);
        return okWithAdditionalParticipant(kTestRemoteShardId3, 5);
    });

    future.default_timed_get();

    ASSERT(txnRouter.getParticipant(kTestRemoteShardId3));
}

// When several responses bail out, the first failure in time order surfaces.
TEST_F(GatherResponsesTest, FirstBailOutErrorWins) {
    beginTransaction();

    auto future = launchAsync([&]() {
        ASSERT_THROWS_CODE(gatherTwoShards(), AssertionException, ErrorCodes::StaleDbVersion);
    });

    {
        // Deliver the second response only after next() has consumed the first, so the
        // arrival order is deterministic.
        FailPointEnableBlock fp("hangBeforePollResponse", BSON("remotesLeft" << 1));
        onCommand([&](const auto& request) {
            ASSERT_EQ(request.target, kTestRemoteHostAndPort1);
            return errorResponse(staleDbVersionStatus());
        });
        fp->waitForTimesEntered(fp.initialTimesEntered() + 1);
    }
    onCommand([&](const auto& request) {
        ASSERT_EQ(request.target, kTestRemoteHostAndPort2);
        return errorResponse(
            Status(CannotImplicitlyCreateCollectionInfo(kTestNss), "cannot implicitly create"));
    });

    future.default_timed_get();
}

// A participant-metadata failure raised by next() while draining after an earlier bail-out does
// not mask the bail-out: the first failure in time order surfaces.
TEST_F(GatherResponsesTest, BailOutWinsOverLaterMetadataFailure) {
    beginTransaction();
    auto txnRouter = TransactionRouter::get(operationContext());

    auto future = launchAsync([&]() {
        ASSERT_THROWS_CODE(gatherTwoShards(), AssertionException, ErrorCodes::StaleDbVersion);
    });

    {
        // Deliver the second response only after next() has consumed the first, so the
        // arrival order is deterministic.
        FailPointEnableBlock fp("hangBeforePollResponse", BSON("remotesLeft" << 1));
        onCommand([&](const auto& request) {
            ASSERT_EQ(request.target, kTestRemoteHostAndPort1);
            // Error responses still carry participant metadata: this one announces shard3 with
            // term 5, which is recorded before the bail-out is captured.
            return errorResponse(staleDbVersionStatus())
                .addFields(BSON("additionalParticipants"
                                << BSON_ARRAY(BSON("shardId" << kTestRemoteShardId3 << "readOnly"
                                                             << true << "term" << 5LL))));
        });
        fp->waitForTimesEntered(fp.initialTimesEntered() + 1);
    }
    onCommand([&](const auto& request) {
        ASSERT_EQ(request.target, kTestRemoteHostAndPort2);
        // shard3 reports a changed term (6): a NoSuchTransaction metadata failure that next()
        // raises only after draining.
        return okWithAdditionalParticipant(kTestRemoteShardId3, 6);
    });

    future.default_timed_get();

    auto recordedParticipant = txnRouter.getParticipant(kTestRemoteShardId3);
    ASSERT(recordedParticipant);
    ASSERT_EQ(*recordedParticipant->term, 5);
}

// A captured bail-out stops retries before draining: a sibling's retryable network error must
// not schedule a retry and stall the drain on a response that never comes.
TEST_F(GatherResponsesTest, BailOutStopsRetryingBeforeDraining) {
    beginTransaction();

    // Pre-enroll both shards so the sender's requests continue the transaction instead of
    // starting it: startTransaction requests have their retry policy demoted to
    // kStrictlyNotIdempotent, which would mask the retry-stopping behavior under test.
    auto txnRouter = TransactionRouter::get(operationContext());
    txnRouter.attachTxnFieldsIfNeeded(
        operationContext(), kTestRemoteShardId1, BSON("find" << "bar"));
    txnRouter.attachTxnFieldsIfNeeded(
        operationContext(), kTestRemoteShardId2, BSON("find" << "bar"));

    auto future = launchAsync([&]() {
        ASSERT_THROWS_CODE(gatherTwoShards(Shard::RetryPolicy::kIdempotent),
                           AssertionException,
                           ErrorCodes::StaleDbVersion);
    });

    {
        // Hold the drain's next() until the bail-out has stopped retrying; otherwise the sibling's
        // failure can be processed, and its retry scheduled, before stopRetrying() runs.
        FailPointEnableBlock fp("hangBeforePollResponse", BSON("remotesLeft" << 1));
        onCommand([&](const auto& request) {
            ASSERT_EQ(request.target, kTestRemoteHostAndPort1);
            return errorResponse(staleDbVersionStatus());
        });
        fp->waitForTimesEntered(fp.initialTimesEntered() + 1);
    }
    onCommand([&](const auto& request) -> StatusWith<BSONObj> {
        ASSERT_EQ(request.target, kTestRemoteHostAndPort2);
        return Status(ErrorCodes::HostUnreachable, "host unreachable");
    });

    future.default_timed_get();
}

// Outside a transaction there is no participant metadata to enroll, so a bail-out surfaces
// immediately instead of waiting for the siblings still in flight.
TEST_F(GatherResponsesTest, BailOutSurfacesImmediatelyOutsideTransaction) {
    auto future = launchAsync([&]() {
        ASSERT_THROWS_CODE(gatherTwoShards(), AssertionException, ErrorCodes::StaleDbVersion);
    });

    // Only one of the two requests is answered; the gather must not wait for the other.
    onCommand([&](const auto& request) { return errorResponse(staleDbVersionStatus()); });

    future.default_timed_get();
}

}  // namespace
}  // namespace mongo
