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


#include "mongo/executor/executor_integration_test_fixture.h"

#include "mongo/rpc/get_status_from_command_result.h"

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
    StringData fp, BSONObj data) {
    auto resp = runSetupCommandSync(DatabaseName::kAdmin,
                                    BSON("configureFailPoint" << fp << "mode"
                                                              << "alwaysOn"
                                                              << "data" << data));
    return FailPointGuard(fp, this, resp.getField("count").Int());
}

ExecutorIntegrationTestFixture::FailPointGuard ExecutorIntegrationTestFixture::configureFailCommand(
    StringData failCommand,
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
