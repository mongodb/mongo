// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <string_view>

namespace mongo {

namespace executor {

class [[MONGO_MOD_OPEN]] ExecutorIntegrationTestFixture : public mongo::unittest::Test {
public:
    virtual BSONObj runSetupCommandSync(const DatabaseName& db, BSONObj cmdObj) = 0;

    RemoteCommandRequest makeTestCommand(Milliseconds timeout,
                                         BSONObj cmd,
                                         OperationContext* opCtx = nullptr,
                                         bool fireAndForget = false,
                                         boost::optional<ErrorCodes::Error> timeoutCode = {},
                                         boost::optional<UUID> operationKey = {});

    RemoteCommandResponse assertOK(StatusWith<RemoteCommandResponse> swResp);

    HostAndPort getServer() {
        return unittest::getFixtureConnectionString().getServers()[0];
    }

    void killOp(UUID opKey) {
        runSetupCommandSync(DatabaseName::kAdmin,
                            BSON("_killOperations" << 1 << "operationKeys" << BSON_ARRAY(opKey)));
    }

    class FailPointGuard {
    public:
        FailPointGuard(std::string_view fpName,
                       ExecutorIntegrationTestFixture* fixture,
                       int initalTimesEntered)
            : _fpName(fpName), _fixture(fixture), _initialTimesEntered(initalTimesEntered) {}

        FailPointGuard(const FailPointGuard&) = delete;
        FailPointGuard& operator=(const FailPointGuard&) = delete;

        ~FailPointGuard() {
            disable();
        }

        void waitForAdditionalTimesEntered(int count);

        void disable();

    private:
        std::string _fpName;
        ExecutorIntegrationTestFixture* _fixture;
        int _initialTimesEntered;
        bool _disabled{false};
    };


    FailPointGuard configureFailPoint(std::string_view fp, BSONObj data);

    FailPointGuard configureFailCommand(std::string_view failCommand,
                                        boost::optional<ErrorCodes::Error> errorCode = boost::none,
                                        boost::optional<Milliseconds> blockTime = boost::none);

private:
    unittest::MinimumLoggedSeverityGuard networkSeverityGuard{
        logv2::LogComponent::kNetwork,
        logv2::LogSeverity::Debug(NetworkInterface::kDiagnosticLogLevel)};
    unittest::MinimumLoggedSeverityGuard executorSeverityGuard{logv2::LogComponent::kExecutor,
                                                               logv2::LogSeverity::Debug(3)};
};

}  // namespace executor
}  // namespace mongo
