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
#include "mongo/client/streamable_replica_set_monitor_error_handler.h"

#include <boost/optional/optional_io.hpp>

#include "mongo/client/sdam/sdam.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/basic.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
using HandshakeStage = StreamableReplicaSetMonitorErrorHandler::HandshakeStage;
using ErrorActions = StreamableReplicaSetMonitorErrorHandler::ErrorActions;
using Error = ErrorCodes::Error;

class StreamableReplicaSetMonitorErrorHandlerTestFixture : public unittest::Test {
public:
    Status makeStatus(Error error) {
        return Status(error, kErrorMessage);
    }

    void verifyActions(ErrorActions result, ErrorActions expectedResult) {
        ASSERT_EQUALS(result.dropConnections, expectedResult.dropConnections);
        ASSERT_EQUALS(result.requestImmediateCheck, expectedResult.requestImmediateCheck);
        if (expectedResult.isMasterOutcome) {
            ASSERT_FALSE(result.isMasterOutcome->isSuccess());
            std::string resultError = result.isMasterOutcome->getErrorMsg();
            ASSERT_NOT_EQUALS(resultError.find(expectedResult.isMasterOutcome->getErrorMsg()),
                              std::string::npos);
        } else {
            ASSERT_FALSE(result.isMasterOutcome);
        }
    }

    void testScenario(HandshakeStage stage,
                      bool isApplicationOperation,
                      std::vector<Error> errors,
                      std::function<ErrorActions(const Status&)> expectedResultGenerator,
                      int numAttempts = 1) {
        auto testSubject = subject();

        const auto prePost = (stage == HandshakeStage::kPreHandshake) ? "pre" : "post";
        const auto applicationOperation = (isApplicationOperation) ? "application" : "monitoring";
        LOGV2_INFO(4712105,
                   "Check Scenario",
                   "handshake"_attr = prePost,
                   "operationType"_attr = applicationOperation);
        for (auto error : errors) {
            LOGV2_INFO(4712106, "Check error ", "error"_attr = ErrorCodes::errorString(error));
            for (int attempt = 0; attempt < numAttempts; attempt++) {
                auto result = testSubject->computeErrorActions(
                    kHost, makeStatus(error), stage, isApplicationOperation, kErrorBson);
                verifyActions(result, expectedResultGenerator(makeStatus(error)));
                LOGV2_INFO(4712107, "Attempt Successful", "num"_attr = attempt);
            }
        }
    }

    void testScenario(HandshakeStage stage,
                      bool isApplicationOperation,
                      std::vector<Error> errors,
                      ErrorActions expectedResult,
                      int numAttempts = 1) {
        testScenario(stage, isApplicationOperation, errors, [expectedResult](const Status&) {
            return expectedResult;
        });
    }

    std::unique_ptr<StreamableReplicaSetMonitorErrorHandler> subject() {
        return std::make_unique<SdamErrorHandler>(kSetName);
    }

    inline static const std::vector<Error> kNetworkErrors{ErrorCodes::SocketException,
                                                          ErrorCodes::NetworkTimeout,
                                                          ErrorCodes::HostNotFound,
                                                          ErrorCodes::HostUnreachable};
    inline static const std::vector<Error> kNetworkErrorsNoTimeout{
        ErrorCodes::SocketException, ErrorCodes::HostNotFound, ErrorCodes::HostUnreachable};
    inline static const std::vector<Error> kInternalError{ErrorCodes::InternalError};
    inline static const std::vector<Error> kNotMasterAndNodeRecovering{
        ErrorCodes::InterruptedAtShutdown,
        ErrorCodes::InterruptedDueToReplStateChange,
        ErrorCodes::NotMasterOrSecondary,
        ErrorCodes::PrimarySteppedDown,
        ErrorCodes::ShutdownInProgress,
        ErrorCodes::NotMaster,
        ErrorCodes::NotMasterNoSlaveOk};

    inline static const std::string kSetName = "setName";
    inline static const HostAndPort kHost = HostAndPort("foobar:123");
    inline static const std::string kErrorMessage = "an error message";
    inline static const BSONObj kErrorBson = BSONObjBuilder().append("ok", 0).obj();
    inline static const sdam::IsMasterOutcome kErrorIsMasterOutcome =
        sdam::IsMasterOutcome(kHost.toString(), kErrorBson, kErrorMessage);

    static constexpr bool kApplicationOperation = true;
    static constexpr bool kMonitoringOperation = false;

    static constexpr int kThreeAttempts = 3;
};

TEST_F(StreamableReplicaSetMonitorErrorHandlerTestFixture, ApplicationNetworkErrorsPreHandshake) {
    testScenario(
        HandshakeStage::kPreHandshake,
        kApplicationOperation,
        kNetworkErrors,
        StreamableReplicaSetMonitorErrorHandler::ErrorActions{true, false, kErrorIsMasterOutcome});
};

// https://github.com/mongodb/specifications/blob/master/source/server-discovery-and-monitoring/server-discovery-and-monitoring.rst#network-error-when-reading-or-writing
TEST_F(StreamableReplicaSetMonitorErrorHandlerTestFixture, ApplicationNetworkErrorsPostHandshake) {
    testScenario(
        HandshakeStage::kPostHandshake,
        kApplicationOperation,
        kNetworkErrorsNoTimeout,
        StreamableReplicaSetMonitorErrorHandler::ErrorActions{true, false, kErrorIsMasterOutcome});
};

// https://github.com/mongodb/specifications/blob/master/source/server-discovery-and-monitoring/server-monitoring.rst#network-error-during-server-check
TEST_F(StreamableReplicaSetMonitorErrorHandlerTestFixture, MonitoringNetworkErrorsPostHandshake) {
    // Two consecutive errors must occur to expect an unknown server description.
    const auto errorServerDescriptionOnSecondNetworkFailure = [](const Status& status) {
        static int count = 0;
        count = (count + 1) % 2;
        return (count == 1)
            ? StreamableReplicaSetMonitorErrorHandler::ErrorActions{true, true, boost::none}
            : StreamableReplicaSetMonitorErrorHandler::ErrorActions{
                  true, false, kErrorIsMasterOutcome};
    };

    testScenario(HandshakeStage::kPostHandshake,
                 kMonitoringOperation,
                 kNetworkErrors,
                 errorServerDescriptionOnSecondNetworkFailure,
                 kThreeAttempts);
}

// https://github.com/mongodb/specifications/blob/master/source/server-discovery-and-monitoring/server-monitoring.rst#network-error-during-server-check
TEST_F(StreamableReplicaSetMonitorErrorHandlerTestFixture, MonitoringNetworkErrorsPreHandshake) {
    testScenario(
        HandshakeStage::kPreHandshake,
        kMonitoringOperation,
        kNetworkErrors,
        StreamableReplicaSetMonitorErrorHandler::ErrorActions{true, false, kErrorIsMasterOutcome},
        kThreeAttempts);
}

// https://github.com/mongodb/specifications/blob/master/source/server-discovery-and-monitoring/server-discovery-and-monitoring.rst#not-master-and-node-is-recovering
TEST_F(StreamableReplicaSetMonitorErrorHandlerTestFixture, ApplicationNotMasterOrNodeRecovering) {
    const auto shutdownErrorsDropConnections = [](const Status& status) {
        if (ErrorCodes::isA<ErrorCategory::ShutdownError>(status.code())) {
            return StreamableReplicaSetMonitorErrorHandler::ErrorActions{
                true, true, kErrorIsMasterOutcome};
        } else {
            return StreamableReplicaSetMonitorErrorHandler::ErrorActions{
                false, true, kErrorIsMasterOutcome};
        }
    };

    testScenario(HandshakeStage::kPostHandshake,
                 kApplicationOperation,
                 kNotMasterAndNodeRecovering,
                 shutdownErrorsDropConnections);
}

TEST_F(StreamableReplicaSetMonitorErrorHandlerTestFixture, MonitoringNonNetworkError) {
    testScenario(
        HandshakeStage::kPostHandshake,
        kMonitoringOperation,
        kInternalError,
        StreamableReplicaSetMonitorErrorHandler::ErrorActions{false, false, kErrorIsMasterOutcome});
}

TEST_F(StreamableReplicaSetMonitorErrorHandlerTestFixture,
       ApplicationNonNetworkIsMasterOrRecoveringError) {
    testScenario(
        HandshakeStage::kPostHandshake,
        kMonitoringOperation,
        kInternalError,
        StreamableReplicaSetMonitorErrorHandler::ErrorActions{false, false, kErrorIsMasterOutcome});
}
}  // namespace mongo
