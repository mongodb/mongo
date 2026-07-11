// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/client/streamable_replica_set_monitor_error_handler.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"

#include <functional>
#include <memory>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
using TriggerEvent = StreamableReplicaSetMonitorErrorHandler::TriggerEvent;
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
        if (expectedResult.helloOutcome) {
            ASSERT_FALSE(result.helloOutcome->isSuccess());
            std::string resultError = result.helloOutcome->getErrorMsg();
            ASSERT_NOT_EQUALS(resultError.find(expectedResult.helloOutcome->getErrorMsg()),
                              std::string::npos);
        } else {
            ASSERT_FALSE(result.helloOutcome);
        }
    }

    void testScenario(TriggerEvent triggerEvent,
                      std::vector<Error> errors,
                      std::function<ErrorActions(const Status&)> expectedResultGenerator,
                      const BSONObj& errorResponse,
                      int numAttempts = 1) {
        auto testSubject = subject();

        LOGV2_INFO(4712105, "Check Scenario", "event"_attr = triggerEvent);
        for (auto error : errors) {
            LOGV2_INFO(4712106, "Check error", "error"_attr = ErrorCodes::errorString(error));
            for (int attempt = 0; attempt < numAttempts; attempt++) {
                auto result = testSubject->computeErrorActions(
                    kHost, makeStatus(error), triggerEvent, errorResponse);
                verifyActions(result, expectedResultGenerator(makeStatus(error)));
                LOGV2_INFO(4712107, "Attempt Successful", "num"_attr = attempt);
            }
        }
    }

    void testScenario(TriggerEvent triggerEvent,
                      std::vector<Error> errors,
                      ErrorActions expectedResult,
                      const BSONObj& errorResponse,
                      int numAttempts = 1) {
        testScenario(
            triggerEvent,
            errors,
            [expectedResult](const Status&) { return expectedResult; },
            errorResponse,
            numAttempts);
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
        ErrorCodes::NotPrimaryOrSecondary,
        ErrorCodes::PrimarySteppedDown,
        ErrorCodes::ShutdownInProgress,
        ErrorCodes::NotWritablePrimary,
        ErrorCodes::NotPrimaryNoSecondaryOk};

    inline static const std::string kSetName = "setName";
    inline static const HostAndPort kHost = HostAndPort("foobar:123");
    inline static const std::string kErrorMessage = "an error message";
    inline static const BSONObj kErrorBson = BSONObjBuilder().append("ok", 0).obj();
    inline static const sdam::HelloOutcome kErrorHelloOutcome =
        sdam::HelloOutcome(kHost, kErrorBson, kErrorMessage);

    static constexpr int kThreeAttempts = 3;
};

TEST_F(StreamableReplicaSetMonitorErrorHandlerTestFixture, ApplicationNetworkErrorsPreHandshake) {
    testScenario(
        TriggerEvent::kApplicationPreHandshake,
        kNetworkErrors,
        StreamableReplicaSetMonitorErrorHandler::ErrorActions{true, false, kErrorHelloOutcome},
        kErrorBson);
};

// https://github.com/mongodb/specifications/blob/master/source/server-discovery-and-monitoring/server-discovery-and-monitoring.rst#network-error-when-reading-or-writing
TEST_F(StreamableReplicaSetMonitorErrorHandlerTestFixture, ApplicationNetworkErrorsPostHandshake) {
    testScenario(
        TriggerEvent::kApplicationPostHandshake,
        kNetworkErrorsNoTimeout,
        StreamableReplicaSetMonitorErrorHandler::ErrorActions{true, false, kErrorHelloOutcome},
        kErrorBson);
};

// https://github.com/mongodb/specifications/blob/master/source/server-discovery-and-monitoring/server-monitoring.rst#network-error-during-server-check
TEST_F(StreamableReplicaSetMonitorErrorHandlerTestFixture, MonitoringNetworkErrorsPostHandshake) {
    // Two consecutive errors must occur to expect an unknown server description.
    // Network errors have no server response, so bson is empty (not ok:0).
    const auto errorServerDescriptionOnSecondNetworkFailure = [](const Status& status) {
        static int count = 0;
        count = (count + 1) % 2;
        return (count == 1)
            ? StreamableReplicaSetMonitorErrorHandler::ErrorActions{true, true, boost::none}
            : StreamableReplicaSetMonitorErrorHandler::ErrorActions{
                  true, false, kErrorHelloOutcome};
    };

    testScenario(TriggerEvent::kHeartbeatFailure,
                 kNetworkErrors,
                 errorServerDescriptionOnSecondNetworkFailure,
                 BSONObj(),  // Local errors don't send a response.
                 kThreeAttempts);
}

// https://github.com/mongodb/specifications/blob/master/source/server-discovery-and-monitoring/server-monitoring.rst#network-error-during-server-check
TEST_F(StreamableReplicaSetMonitorErrorHandlerTestFixture, MonitoringNetworkErrorsPreHandshake) {
    testScenario(
        TriggerEvent::kHandshakeFailure,
        kNetworkErrors,
        StreamableReplicaSetMonitorErrorHandler::ErrorActions{true, false, kErrorHelloOutcome},
        BSONObj(),  // Local errors don't send a response.
        kThreeAttempts);
}

// https://github.com/mongodb/specifications/blob/master/source/server-discovery-and-monitoring/server-discovery-and-monitoring.rst#not-master-and-node-is-recovering
TEST_F(StreamableReplicaSetMonitorErrorHandlerTestFixture, ApplicationNotMasterOrNodeRecovering) {
    const auto shutdownErrorsDropConnections = [](const Status& status) {
        if (ErrorCodes::isA<ErrorCategory::ShutdownError>(status.code())) {
            return StreamableReplicaSetMonitorErrorHandler::ErrorActions{
                true, true, kErrorHelloOutcome};
        } else {
            return StreamableReplicaSetMonitorErrorHandler::ErrorActions{
                false, true, kErrorHelloOutcome};
        }
    };

    testScenario(TriggerEvent::kApplicationPostHandshake,
                 kNotMasterAndNodeRecovering,
                 shutdownErrorsDropConnections,
                 kErrorBson);
}

TEST_F(StreamableReplicaSetMonitorErrorHandlerTestFixture, MonitoringNonNetworkError) {
    testScenario(
        TriggerEvent::kHeartbeatFailure,
        kInternalError,
        StreamableReplicaSetMonitorErrorHandler::ErrorActions{false, false, kErrorHelloOutcome},
        BSONObj());  // Local errors don't send a response.
}

TEST_F(StreamableReplicaSetMonitorErrorHandlerTestFixture,
       MonitoringNonNetworkHelloOrRecoveringError) {
    testScenario(
        TriggerEvent::kHeartbeatFailure,
        kInternalError,
        StreamableReplicaSetMonitorErrorHandler::ErrorActions{false, false, kErrorHelloOutcome},
        BSONObj());  // Local errors don't send a response.
}

TEST_F(StreamableReplicaSetMonitorErrorHandlerTestFixture, MonitoringRemoteHelloError) {
    for (auto triggerEvent : {TriggerEvent::kHeartbeatFailure, TriggerEvent::kHandshakeFailure}) {
        auto testSubject = subject();
        const auto status = makeStatus(ErrorCodes::ShutdownInProgress);
        const auto bsonWithCode =
            BSONObjBuilder()
                .append("ok", 0)
                .append("code", static_cast<int>(ErrorCodes::ShutdownInProgress))
                .obj();

        auto result = testSubject->computeErrorActions(kHost, status, triggerEvent, bsonWithCode);
        verifyActions(result,
                      StreamableReplicaSetMonitorErrorHandler::ErrorActions{
                          false, false, kErrorHelloOutcome});
    }
}
}  // namespace mongo
