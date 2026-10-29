// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/executor/executor_integration_test_fixture.h"

#include "mongo/rpc/get_status_from_command_result.h"

#include <string_view>

namespace mongo::executor {

RemoteCommandRequest ExecutorIntegrationTestFixture::makeTestCommand(
    Milliseconds timeout,
    BSONObj cmd,
    OperationContext* opCtx,
    bool fireAndForget,
    boost::optional<ErrorCodes::Error> timeoutCode,
    boost::optional<UUID> operationKey) {
    if (!operationKey) {
        operationKey = UUID::gen();
    }

    cmd = cmd.addField(
        BSON(GenericArguments::kClientOperationKeyFieldName << *operationKey).firstElement());

    RemoteCommandRequest request(getServer(),
                                 DatabaseName::kAdmin,
                                 std::move(cmd),
                                 BSONObj(),
                                 opCtx,
                                 timeout,
                                 fireAndForget,
                                 operationKey);
    // Don't override possible opCtx error code.
    if (timeoutCode) {
        request.timeoutCode = timeoutCode;
    }
    return request;
}

RemoteCommandResponse ExecutorIntegrationTestFixture::assertOK(
    StatusWith<RemoteCommandResponse> swResp) {
    ASSERT_OK(swResp);
    ASSERT_OK(swResp.getValue().status);
    ASSERT_OK(getStatusFromCommandResult(swResp.getValue().data));
    return swResp.getValue();
}

ExecutorIntegrationTestFixture::FailPointGuard ExecutorIntegrationTestFixture::configureFailPoint(
    std::string_view fp, BSONObj data) {
    auto resp = runSetupCommandSync(DatabaseName::kAdmin,
                                    BSON("configureFailPoint" << fp << "mode"
                                                              << "alwaysOn"
                                                              << "data" << data));
    return FailPointGuard(fp, this, resp.getField("count").Int());
}

ExecutorIntegrationTestFixture::FailPointGuard ExecutorIntegrationTestFixture::configureFailCommand(
    std::string_view failCommand,
    boost::optional<ErrorCodes::Error> errorCode,
    boost::optional<Milliseconds> blockTime) {
    auto data = BSON("failCommands" << BSON_ARRAY(failCommand));

    if (errorCode) {
        data = data.addField(BSON("errorCode" << *errorCode).firstElement());
    }

    if (blockTime) {
        data =
            data.addFields(BSON("blockConnection" << true << "blockTimeMS" << blockTime->count()));
    }
    return configureFailPoint("failCommand", data);
}

void ExecutorIntegrationTestFixture::FailPointGuard::waitForAdditionalTimesEntered(int count) {
    auto cmdObj = BSON("waitForFailPoint" << _fpName << "timesEntered"
                                          << _initialTimesEntered + count << "maxTimeMS" << 30000);
    _fixture->runSetupCommandSync(DatabaseName::kAdmin, cmdObj);
}

void ExecutorIntegrationTestFixture::FailPointGuard::disable() {
    if (std::exchange(_disabled, true)) {
        return;
    }

    auto cmdObj = BSON("configureFailPoint" << _fpName << "mode"
                                            << "off");
    _fixture->runSetupCommandSync(DatabaseName::kAdmin, cmdObj);
}
}  // namespace mongo::executor
