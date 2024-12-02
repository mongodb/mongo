/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <memory>

#include "mongo/base/error_codes.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/transport/grpc/grpc_session.h"
#include "mongo/transport/grpc/test_fixtures.h"
#include "mongo/transport/grpc/util.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/scopeguard.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::transport::grpc {
namespace {

class GRPCSessionTest : public ServiceContextWithClockSourceMockTest {
public:
    static constexpr auto kStreamTimeout = Seconds(1);
    static constexpr auto kClientId = "c08663ac-2f6c-408d-8829-97e67eef9f23";

    void setUp() override {
        ServiceContextWithClockSourceMockTest::setUp();
        _streamFixtures = _makeStreamFixtures();
        _reactor = std::make_shared<GRPCReactor>();
    }

    void tearDown() override {
        _streamFixtures.reset();
        ServiceContextWithClockSourceMockTest::tearDown();
    }

    std::unique_ptr<IngressSession> makeIngressSession(boost::optional<UUID> remoteClientId) {
        return std::make_unique<IngressSession>(nullptr,
                                                _streamFixtures->rpc->serverCtx.get(),
                                                _streamFixtures->rpc->serverStream.get(),
                                                std::move(remoteClientId),
                                                /* auth token */ boost::none,
                                                /* client metadata */ boost::none);
    }

    std::unique_ptr<EgressSession> makeEgressSession(UUID clientId) {
        return std::make_unique<EgressSession>(nullptr,
                                               _reactor,
                                               _streamFixtures->clientCtx,
                                               _streamFixtures->clientStream,
                                               std::move(clientId),
                                               /* shared state */ nullptr);
    }

    template <class SessionType>
    auto makeSession(UUID clientId) {
        if constexpr (std::is_same<SessionType, IngressSession>::value) {
            return makeIngressSession(clientId);
        } else {
            static_assert(std::is_same<SessionType, EgressSession>::value == true);
            return makeEgressSession(clientId);
        }
    }

    template <class SessionType>
    auto makeSession() {
        return makeSession<SessionType>(uassertStatusOK(UUID::parse(kClientId)));
    }

    /**
     * Runs the provided callback twice, each time with a new fixture:
     * - First with an instance of `EgressSession`.
     * - Then with an instance of `IngressSession`.
     * Upon completion, `_streamFixtures` is restored to its original state.
     */
    using CallbackType = std::function<void(MockRPC&, GRPCSession&)>;
    void runWithBoth(CallbackType cb) {
        _runCallback<EgressSession>(cb);
        _runCallback<IngressSession>(cb);
    }

    auto& fixtures() const {
        return *_streamFixtures;
    }

    /**
     * Tests that the effects of cancellation are properly reported both locally and remotely.
     * The provided lambda should return a pair of the two sessions: the first being the session
     * that will be cancelled.
     */
    void runCancellationTest(
        std::function<std::pair<GRPCSession&, GRPCSession&>(IngressSession&, EgressSession&)>
            whichSession) {
        auto ingressSession = makeSession<IngressSession>();
        auto egressSession = makeSession<EgressSession>();
        auto cancellationReason = Status(ErrorCodes::ShutdownInProgress, "shutdown error");

        ASSERT_TRUE(ingressSession->isConnected());
        ASSERT_TRUE(egressSession->isConnected());

        auto [sessionToCancel, other] = whichSession(*ingressSession, *egressSession);
        sessionToCancel.cancel(cancellationReason);
        ASSERT_EQ(sessionToCancel.terminationStatus(), cancellationReason);

        ASSERT_FALSE(ingressSession->isConnected());
        ASSERT_NOT_OK(ingressSession->terminationStatus());
        ASSERT_TRUE(fixtures().rpc->serverCtx->isCancelled());

        ASSERT_NOT_OK(egressSession->finish());
        ASSERT_TRUE(egressSession->terminationStatus());
        ASSERT_FALSE(egressSession->isConnected());

        ASSERT_TRUE(ErrorCodes::isCancellationError(*other.terminationStatus()));
    }

private:
    std::unique_ptr<MockStreamTestFixtures> _makeStreamFixtures() {
        // The MockStreamTestFixtures created here doesn't contain any references to the channel or
        // server, so it's okay to let stubFixture go out of scope.
        MockStubTestFixtures stubFixture;
        MetadataView metadata = {{"foo", "bar"}};
        return stubFixture.makeStreamTestFixtures(
            getServiceContext()->getFastClockSource()->now() + kStreamTimeout, std::move(metadata));
    }

    template <class SessionType>
    void _runCallback(CallbackType& cb) {
        // Install a new fixture for the duration of this call.
        auto localFixtures = _makeStreamFixtures();
        _streamFixtures.swap(localFixtures);
        ON_BLOCK_EXIT([&] { _streamFixtures.swap(localFixtures); });

        auto session = makeSession<SessionType>();
        ON_BLOCK_EXIT([&] { session->end(); });
        LOGV2(7401431,
              "Running test with gRPC session",
              "type"_attr = std::is_same<SessionType, EgressSession>::value ? "egress" : "ingress");
        cb(*_streamFixtures->rpc, *session);
    }

    std::unique_ptr<MockStreamTestFixtures> _streamFixtures;
    std::shared_ptr<GRPCReactor> _reactor;
};

TEST_F(GRPCSessionTest, NoClientId) {
    auto session = makeIngressSession(boost::none);
    ASSERT_FALSE(session->getRemoteClientId());
    session->end();
}

TEST_F(GRPCSessionTest, GetClientId) {
    {
        auto session = makeSession<IngressSession>();
        ASSERT_TRUE(session->getRemoteClientId());
        ASSERT_EQ(session->getRemoteClientId()->toString(), kClientId);
    }

    {
        auto session = makeSession<EgressSession>();
        ASSERT_EQ(session->getClientId().toString(), kClientId);
    }
}

TEST_F(GRPCSessionTest, GetRemote) {
    runWithBoth([&](auto&, auto& session) {
        auto expectedRemote = dynamic_cast<EgressSession*>(&session)
            ? MockStubTestFixtures::kBindAddress
            : MockStubTestFixtures::kClientAddress;
        ASSERT_EQ(session.remote(), HostAndPort(expectedRemote));
    });
}

TEST_F(GRPCSessionTest, CancelIngress) {
    runCancellationTest([](IngressSession& ingress, EgressSession& egress) {
        return std::pair<GRPCSession&, GRPCSession&>(ingress, egress);
    });
}

TEST_F(GRPCSessionTest, CancelEgress) {
    runCancellationTest([](IngressSession& ingress, EgressSession& egress) {
        return std::pair<GRPCSession&, GRPCSession&>(egress, ingress);
    });
}

TEST_F(GRPCSessionTest, End) {
    runWithBoth([&](auto& rpc, auto& session) {
        session.end();
        ASSERT_FALSE(session.isConnected());
        ASSERT_TRUE(session.terminationStatus());
        ASSERT_EQ(session.terminationStatus()->code(), ErrorCodes::CallbackCanceled);
        ASSERT_TRUE(rpc.serverCtx->isCancelled());
    });
}

TEST_F(GRPCSessionTest, CancelWithReason) {
    Status kExpectedReason = Status(ErrorCodes::ShutdownInProgress, "Some error condition");
    runWithBoth([&](auto& rpc, auto& session) {
        session.cancel(kExpectedReason);
        ASSERT_FALSE(session.isConnected());
        ASSERT_TRUE(session.terminationStatus());
        ASSERT_EQ(session.terminationStatus(), kExpectedReason);
        ASSERT_EQ(session.sourceMessage().getStatus(), kExpectedReason.code());
        ASSERT_EQ(session.sinkMessage(makeUniqueMessage()), kExpectedReason.code());
        ASSERT_TRUE(rpc.serverCtx->isCancelled());
    });
}

TEST_F(GRPCSessionTest, TerminationStatusIsNotOverridden) {
    Status kExpectedReason = Status(ErrorCodes::ShutdownInProgress, "Some error condition");
    runWithBoth([&](auto&, auto& session) {
        session.cancel(kExpectedReason);

        // Cancelling the session again should have no effect on the reason.
        session.cancel(Status(ErrorCodes::CallbackCanceled, "second reason"));
        ASSERT_EQ(session.terminationStatus(), kExpectedReason);

        // Ending the session again should have no effect either.
        session.end();
        ASSERT_EQ(session.terminationStatus(), kExpectedReason);

        if (auto egressSession = dynamic_cast<EgressSession*>(&session)) {
            // EgressSession::finish() should report the proper status.
            auto finishStatus = egressSession->finish();
            ASSERT_EQ(finishStatus, kExpectedReason);
        } else if (auto ingressSession = dynamic_cast<IngressSession*>(&session)) {
            // Recording the status should not overwrite the prior cancellation status.
            ingressSession->setTerminationStatus(Status::OK());
            ASSERT_EQ(session.terminationStatus(), kExpectedReason);
        }
    });
}

TEST_F(GRPCSessionTest, ReadAndWrite) {
    auto ingressSession = makeSession<IngressSession>();
    auto egressSession = makeSession<EgressSession>();
    ON_BLOCK_EXIT([&] {
        ingressSession->end();
        egressSession->end();
    });

    auto sendMessage = [&](Session& sender, Session& receiver) {
        auto msg = makeUniqueMessage();
        ASSERT_OK(sender.sinkMessage(msg));
        auto swReceived = receiver.sourceMessage();
        ASSERT_OK(swReceived.getStatus());
        ASSERT_EQ_MSG(swReceived.getValue(), msg);
    };

    sendMessage(*egressSession, *ingressSession);
    sendMessage(*ingressSession, *egressSession);
}

enum class Operation { kSink, kSource };
Status runDummyOperationOnSession(Session& session, Operation op) {
    if (op == Operation::kSink) {
        return session.sinkMessage({});
    } else {
        return session.sourceMessage().getStatus();
    }
}

TEST_F(GRPCSessionTest, ReadAndWriteFromClosedStream) {
    for (auto op : {Operation::kSink, Operation::kSource}) {
        runWithBoth([&](auto&, auto& session) {
            session.end();
            ASSERT_EQ(runDummyOperationOnSession(session, op), ErrorCodes::CallbackCanceled);
        });
    }
}

TEST_F(GRPCSessionTest, ReadAndWriteTimesOut) {
    for (auto op : {Operation::kSink, Operation::kSource}) {
        runWithBoth([&](auto&, auto& session) {
            clockSource().advance(2 * kStreamTimeout);

            if (auto egressSession = dynamic_cast<EgressSession*>(&session)) {
                // Verify that the right `ErrorCode` is delivered on the client-side.
                ASSERT_EQ(runDummyOperationOnSession(session, op), ErrorCodes::ExceededTimeLimit);
                ASSERT_TRUE(ErrorCodes::isExceededTimeLimitError(egressSession->finish()));
            } else {
                ASSERT_EQ(runDummyOperationOnSession(session, op), ErrorCodes::CallbackCanceled);
            }
        });
    }
}

TEST_F(GRPCSessionTest, FinishOK) {
    auto session = makeSession<EgressSession>();
    fixtures().rpc->sendReturnStatus(::grpc::Status::OK);
    ASSERT_OK(session->finish());
}

TEST_F(GRPCSessionTest, FinishError) {
    auto session = makeSession<EgressSession>();
    ::grpc::Status error{::grpc::UNAVAILABLE, "connection error"};
    fixtures().rpc->sendReturnStatus(error);
    ASSERT_EQ(session->finish(), util::convertStatus(error));
}

TEST_F(GRPCSessionTest, FinishCancelled) {
    auto ingressSession = makeSession<IngressSession>();
    auto egressSession = makeSession<EgressSession>();
    auto cancellationReason = Status(ErrorCodes::ShutdownInProgress, "some reason");
    ingressSession->cancel(cancellationReason);
    auto finishStatus = egressSession->finish();
    ASSERT_TRUE(ErrorCodes::isCancellationError(finishStatus));
    ASSERT_EQ(ingressSession->terminationStatus(), cancellationReason);
}

}  // namespace
}  // namespace mongo::transport::grpc
