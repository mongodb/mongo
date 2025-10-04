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

#include "mongo/db/auth/authentication_session.h"

#include "mongo/base/status_with.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/thread_name.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

class AuthenticationSessionTestFixture : public ServiceContextTest {
public:
    AuthenticationSessionTestFixture() {
        Client::releaseCurrent();
        Client::initThread(getThreadName(), getGlobalServiceContext()->getService(), session);
        opCtx = cc().makeOperationContext();
    }

    transport::TransportLayerMock transportLayer;
    std::shared_ptr<transport::Session> session = transportLayer.createSession();
    ServiceContext::UniqueOperationContext opCtx;
};

// Helper function to initialize a session using SaslStart and manually set the mechanism.
void initSessionAndSetMech(OperationContext* opCtx) {
    auto guard =
        AuthenticationSession::StepGuard(opCtx, AuthenticationSession::StepType::kSaslStart);
    auto session = guard.getSession();

    // Arbitrarily set the mechanism to SCRAM-SHA-1 so that the session's mechanism has a value.
    auto mechanism = SASLServerMechanismRegistry::get(opCtx->getService())
                         .getServerMechanism("SCRAM-SHA-1", "testDB");
    session->setMechanism(std::move(mechanism.getValue()), boost::none);
};

// In the case where a normal authentication follows a speculative one, we want to ensure that the
// session persisted but the authentication mechanism has been reset.
void checkSpecAuthFollowedByAuthCase(AuthenticationSession* session,
                                     AuthenticationSession::StepType firstStep,
                                     AuthenticationSession::StepType secondStep) {
    bool firstStepIsSpec = firstStep == AuthenticationSession::StepType::kSpeculativeAuthenticate ||
        firstStep == AuthenticationSession::StepType::kSpeculativeSaslStart;
    bool secondStepIsAuth = secondStep == AuthenticationSession::StepType::kAuthenticate ||
        secondStep == AuthenticationSession::StepType::kSaslStart;
    if (firstStepIsSpec && secondStepIsAuth) {
        ASSERT_FALSE(session->isSpeculative());
        ASSERT(session->getMechanismName() == "");
        ASSERT(!session->getMechanism());
    }
}

// Initializes two StepGuards for each given step to ensure that the correct behaviour ensues
// between the two steps.
void authenticationSessionTestHelper(OperationContext* opCtx,
                                     AuthenticationSession::StepType firstStep,
                                     AuthenticationSession::StepType secondStep,
                                     bool secondStepShouldCauseOverride = false,
                                     bool secondStepShouldThrowException = false) {
    // Execute the first step.
    if (firstStep == AuthenticationSession::StepType::kSaslStart) {
        // In addition to running kSaslStart, we need to ensure that the mechanism is set for the
        // first step in the case that the second step is kSaslContinue (which requires this).
        initSessionAndSetMech(opCtx);
    } else {
        auto guard = AuthenticationSession::StepGuard(opCtx, firstStep);
    }

    // Execute the second step.
    if (secondStepShouldThrowException) {
        ASSERT_THROWS_CODE(AuthenticationSession::StepGuard(opCtx, secondStep),
                           DBException,
                           ErrorCodes::ProtocolError);
    } else {
        auto guard = AuthenticationSession::StepGuard(opCtx, secondStep);
        auto session = guard.getSession();

        if (secondStepShouldCauseOverride) {
            // If the session was overridden by a new session, ensure that a new session was indeed
            // created and that the first one was finished by ensuring that the current session's
            // last step has not been set.
            ASSERT(!session->getLastStep());
        } else {
            // If the session was supposed to persist between the first and second steps, ensure
            // that the last step is indeed firstStep.
            ASSERT(session->getLastStep() == firstStep);

            checkSpecAuthFollowedByAuthCase(session, firstStep, secondStep);
        }
    }
}

TEST_F(AuthenticationSessionTestFixture, TestSaslSupportedMechsFollowedBySaslSupportedMechs) {
    authenticationSessionTestHelper(opCtx.get(),
                                    AuthenticationSession::StepType::kSaslSupportedMechanisms,
                                    AuthenticationSession::StepType::kSaslSupportedMechanisms,
                                    true /* secondStepShouldCauseOverride */);
}

TEST_F(AuthenticationSessionTestFixture, TestSaslSupportedMechsFollowedByAuth) {
    authenticationSessionTestHelper(opCtx.get(),
                                    AuthenticationSession::StepType::kSaslSupportedMechanisms,
                                    AuthenticationSession::StepType::kAuthenticate);
}

TEST_F(AuthenticationSessionTestFixture, TestSaslSupportedMechsFollowedBySaslStart) {
    authenticationSessionTestHelper(opCtx.get(),
                                    AuthenticationSession::StepType::kSaslSupportedMechanisms,
                                    AuthenticationSession::StepType::kSaslStart);
}

TEST_F(AuthenticationSessionTestFixture, TestSaslSupportedMechsFollowedBySaslContinue) {
    // kSaslContinue throws an exception if the previous command was not kSaslStart or
    // kSaslContinue.
    authenticationSessionTestHelper(opCtx.get(),
                                    AuthenticationSession::StepType::kSaslSupportedMechanisms,
                                    AuthenticationSession::StepType::kSaslContinue,
                                    false /* secondStepShouldCauseOverride */,
                                    true /* secondStepShouldThrowException */);
}

TEST_F(AuthenticationSessionTestFixture, TestSaslSupportedMechsFollowedBySpecAuth) {
    authenticationSessionTestHelper(opCtx.get(),
                                    AuthenticationSession::StepType::kSaslSupportedMechanisms,
                                    AuthenticationSession::StepType::kSpeculativeAuthenticate);
}

TEST_F(AuthenticationSessionTestFixture, TestSaslSupportedMechsFollowedBySpecSaslStart) {
    authenticationSessionTestHelper(opCtx.get(),
                                    AuthenticationSession::StepType::kSaslSupportedMechanisms,
                                    AuthenticationSession::StepType::kSpeculativeSaslStart);
}

TEST_F(AuthenticationSessionTestFixture, TestAuthFollowedBySaslSupportedMechs) {
    authenticationSessionTestHelper(opCtx.get(),
                                    AuthenticationSession::StepType::kAuthenticate,
                                    AuthenticationSession::StepType::kSaslSupportedMechanisms,
                                    true /* secondStepShouldCauseOverride */);
}

TEST_F(AuthenticationSessionTestFixture, TestAuthFollowedByAuth) {
    authenticationSessionTestHelper(opCtx.get(),
                                    AuthenticationSession::StepType::kAuthenticate,
                                    AuthenticationSession::StepType::kAuthenticate,
                                    true /* secondStepShouldCauseOverride */);
}

TEST_F(AuthenticationSessionTestFixture, TestAuthFollowedBySaslStart) {
    authenticationSessionTestHelper(opCtx.get(),
                                    AuthenticationSession::StepType::kAuthenticate,
                                    AuthenticationSession::StepType::kSaslStart,
                                    true /* secondStepShouldCauseOverride */);
}

TEST_F(AuthenticationSessionTestFixture, TestAuthFollowedBySaslContinue) {
    // kSaslContinue throws an exception if the previous command was not kSaslStart or
    // kSaslContinue.
    authenticationSessionTestHelper(opCtx.get(),
                                    AuthenticationSession::StepType::kAuthenticate,
                                    AuthenticationSession::StepType::kSaslContinue,
                                    false /* secondStepShouldCauseOverride */,
                                    true /* secondStepShouldThrowException */);
}

TEST_F(AuthenticationSessionTestFixture, TestAuthFollowedBySpecAuth) {
    authenticationSessionTestHelper(opCtx.get(),
                                    AuthenticationSession::StepType::kAuthenticate,
                                    AuthenticationSession::StepType::kSpeculativeAuthenticate,
                                    true /* secondStepShouldCauseOverride */);
}

TEST_F(AuthenticationSessionTestFixture, TestAuthFollowedBySpecSaslStart) {
    authenticationSessionTestHelper(opCtx.get(),
                                    AuthenticationSession::StepType::kAuthenticate,
                                    AuthenticationSession::StepType::kSpeculativeSaslStart,
                                    true /* secondStepShouldCauseOverride */);
}

TEST_F(AuthenticationSessionTestFixture, TestSaslStartFollowedBySaslSupportedMechs) {
    authenticationSessionTestHelper(opCtx.get(),
                                    AuthenticationSession::StepType::kSaslStart,
                                    AuthenticationSession::StepType::kSaslSupportedMechanisms,
                                    true /* secondStepShouldCauseOverride */);
}

TEST_F(AuthenticationSessionTestFixture, TestSaslStartFollowedByAuth) {
    authenticationSessionTestHelper(opCtx.get(),
                                    AuthenticationSession::StepType::kSaslStart,
                                    AuthenticationSession::StepType::kAuthenticate,
                                    true /* secondStepShouldCauseOverride */);
}

TEST_F(AuthenticationSessionTestFixture, TestSaslStartFollowedBySaslStart) {
    authenticationSessionTestHelper(opCtx.get(),
                                    AuthenticationSession::StepType::kSaslStart,
                                    AuthenticationSession::StepType::kSaslStart,
                                    true /* secondStepShouldCauseOverride */);
}

TEST_F(AuthenticationSessionTestFixture, TestSaslStartFollowedBySaslContinue) {
    authenticationSessionTestHelper(opCtx.get(),
                                    AuthenticationSession::StepType::kSaslStart,
                                    AuthenticationSession::StepType::kSaslContinue);
}

TEST_F(AuthenticationSessionTestFixture, TestSaslStartFollowedBySpecAuth) {
    authenticationSessionTestHelper(opCtx.get(),
                                    AuthenticationSession::StepType::kSaslStart,
                                    AuthenticationSession::StepType::kSpeculativeAuthenticate,
                                    true /* secondStepShouldCauseOverride */);
}

TEST_F(AuthenticationSessionTestFixture, TestSaslStartFollowedBySpecSaslStart) {
    authenticationSessionTestHelper(opCtx.get(),
                                    AuthenticationSession::StepType::kSaslStart,
                                    AuthenticationSession::StepType::kSpeculativeSaslStart,
                                    true /* secondStepShouldCauseOverride */);
}

TEST_F(AuthenticationSessionTestFixture, TestSaslContinueFollowedBySaslSupportedMechs) {
    // Need to initialize the session and set the mechanism before kSaslContinue is run as the first
    // step in order to pass the asserts in the StepGuard constructor.
    initSessionAndSetMech(opCtx.get());
    authenticationSessionTestHelper(opCtx.get(),
                                    AuthenticationSession::StepType::kSaslContinue,
                                    AuthenticationSession::StepType::kSaslSupportedMechanisms,
                                    true /* secondStepShouldCauseOverride */);
}

TEST_F(AuthenticationSessionTestFixture, TestSaslContinueFollowedByAuth) {
    // Need to initialize the session and set the mechanism before kSaslContinue is run as the first
    // step in order to pass the asserts in the StepGuard constructor.
    initSessionAndSetMech(opCtx.get());
    authenticationSessionTestHelper(opCtx.get(),
                                    AuthenticationSession::StepType::kSaslContinue,
                                    AuthenticationSession::StepType::kAuthenticate,
                                    true /* secondStepShouldCauseOverride */);
}

TEST_F(AuthenticationSessionTestFixture, TestSaslContinueFollowedBySaslStart) {
    // Need to initialize the session and set the mechanism before kSaslContinue is run as the first
    // step in order to pass the asserts in the StepGuard constructor.
    initSessionAndSetMech(opCtx.get());
    authenticationSessionTestHelper(opCtx.get(),
                                    AuthenticationSession::StepType::kSaslContinue,
                                    AuthenticationSession::StepType::kSaslStart,
                                    true /* secondStepShouldCauseOverride */);
}

TEST_F(AuthenticationSessionTestFixture, TestSaslContinueFollowedBySaslContinue) {
    // Need to initialize the session and set the mechanism before kSaslContinue is run as the first
    // step in order to pass the asserts in the StepGuard constructor.
    initSessionAndSetMech(opCtx.get());
    authenticationSessionTestHelper(opCtx.get(),
                                    AuthenticationSession::StepType::kSaslContinue,
                                    AuthenticationSession::StepType::kSaslContinue);
}

TEST_F(AuthenticationSessionTestFixture, TestSaslContinueFollowedBySpecAuth) {
    // Need to initialize the session and set the mechanism before kSaslContinue is run as the first
    // step in order to pass the asserts in the StepGuard constructor.
    initSessionAndSetMech(opCtx.get());
    authenticationSessionTestHelper(opCtx.get(),
                                    AuthenticationSession::StepType::kSaslContinue,
                                    AuthenticationSession::StepType::kSpeculativeAuthenticate,
                                    true /* secondStepShouldCauseOverride */);
}

TEST_F(AuthenticationSessionTestFixture, TestSaslContinueFollowedBySpecSaslStart) {
    // Need to initialize the session and set the mechanism before kSaslContinue is run as the first
    // step in order to pass the asserts in the StepGuard constructor.
    initSessionAndSetMech(opCtx.get());
    authenticationSessionTestHelper(opCtx.get(),
                                    AuthenticationSession::StepType::kSaslContinue,
                                    AuthenticationSession::StepType::kSpeculativeSaslStart,
                                    true /* secondStepShouldCauseOverride */);
}

TEST_F(AuthenticationSessionTestFixture, TestSpecAuthFollowedBySaslSupportedMechs) {
    authenticationSessionTestHelper(opCtx.get(),
                                    AuthenticationSession::StepType::kSpeculativeAuthenticate,
                                    AuthenticationSession::StepType::kSaslSupportedMechanisms,
                                    true /* secondStepShouldCauseOverride */);
}

TEST_F(AuthenticationSessionTestFixture, TestSpecAuthFollowedByAuth) {
    authenticationSessionTestHelper(opCtx.get(),
                                    AuthenticationSession::StepType::kSpeculativeAuthenticate,
                                    AuthenticationSession::StepType::kAuthenticate);
}

TEST_F(AuthenticationSessionTestFixture, TestSpecAuthFollowedBySaslStart) {
    authenticationSessionTestHelper(opCtx.get(),
                                    AuthenticationSession::StepType::kSpeculativeAuthenticate,
                                    AuthenticationSession::StepType::kSaslStart);
}

TEST_F(AuthenticationSessionTestFixture, TestSpecAuthFollowedBySaslContinue) {
    // kSaslContinue throws an exception if the previous command was not kSaslStart or
    // kSaslContinue.
    authenticationSessionTestHelper(opCtx.get(),
                                    AuthenticationSession::StepType::kSpeculativeAuthenticate,
                                    AuthenticationSession::StepType::kSaslContinue,
                                    false /* secondStepShouldCauseOverride */,
                                    true /* secondStepShouldThrowException */);
}

TEST_F(AuthenticationSessionTestFixture, TestSpecAuthFollowedBySpecAuth) {
    authenticationSessionTestHelper(opCtx.get(),
                                    AuthenticationSession::StepType::kSpeculativeAuthenticate,
                                    AuthenticationSession::StepType::kSpeculativeAuthenticate,
                                    true /* secondStepShouldCauseOverride */);
}

TEST_F(AuthenticationSessionTestFixture, TestSpecAuthFollowedBySpecSaslStart) {
    authenticationSessionTestHelper(opCtx.get(),
                                    AuthenticationSession::StepType::kSpeculativeAuthenticate,
                                    AuthenticationSession::StepType::kSpeculativeSaslStart,
                                    true /* secondStepShouldCauseOverride */);
}

TEST_F(AuthenticationSessionTestFixture, TestSpecSaslStartFollowedBySaslSupportedMechs) {
    authenticationSessionTestHelper(opCtx.get(),
                                    AuthenticationSession::StepType::kSpeculativeSaslStart,
                                    AuthenticationSession::StepType::kSaslSupportedMechanisms,
                                    true /* secondStepShouldCauseOverride */);
}

TEST_F(AuthenticationSessionTestFixture, TestSpecSaslStartFollowedByAuth) {
    authenticationSessionTestHelper(opCtx.get(),
                                    AuthenticationSession::StepType::kSpeculativeSaslStart,
                                    AuthenticationSession::StepType::kAuthenticate);
}

TEST_F(AuthenticationSessionTestFixture, TestSpecSaslStartFollowedBySaslStart) {
    authenticationSessionTestHelper(opCtx.get(),
                                    AuthenticationSession::StepType::kSpeculativeSaslStart,
                                    AuthenticationSession::StepType::kSaslStart);
}

TEST_F(AuthenticationSessionTestFixture, TestSpecSaslStartFollowedBySaslContinue) {
    // kSaslContinue throws an exception if the previous command was not kSaslStart or
    // kSaslContinue.
    authenticationSessionTestHelper(opCtx.get(),
                                    AuthenticationSession::StepType::kSpeculativeSaslStart,
                                    AuthenticationSession::StepType::kSaslContinue,
                                    false /* secondStepShouldCauseOverride */,
                                    true /* secondStepShouldThrowException */);
}

TEST_F(AuthenticationSessionTestFixture, TestSpecSaslStartFollowedBySpecAuth) {
    authenticationSessionTestHelper(opCtx.get(),
                                    AuthenticationSession::StepType::kSpeculativeSaslStart,
                                    AuthenticationSession::StepType::kSpeculativeAuthenticate,
                                    true /* secondStepShouldCauseOverride */);
}

TEST_F(AuthenticationSessionTestFixture, TestSpecSaslStartFollowedBySpecSaslStart) {
    authenticationSessionTestHelper(opCtx.get(),
                                    AuthenticationSession::StepType::kSpeculativeSaslStart,
                                    AuthenticationSession::StepType::kSpeculativeSaslStart,
                                    true /* secondStepShouldCauseOverride */);
}

}  // namespace
}  // namespace mongo
