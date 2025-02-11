/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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
#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/uuid.h"

namespace mongo {

namespace executor {

class ExecutorIntegrationTestFixture : public mongo::unittest::Test {
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
        FailPointGuard(StringData fpName,
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


    FailPointGuard configureFailPoint(StringData fp, BSONObj data);

    FailPointGuard configureFailCommand(StringData failCommand,
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
