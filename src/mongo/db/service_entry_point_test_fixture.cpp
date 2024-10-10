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


#include "mongo/db/service_entry_point_test_fixture.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobj_comparator.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/curop.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/read_write_concern_defaults_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/transport/mock_session.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/tick_source_mock.h"

namespace mongo {

void ServiceEntryPointTestFixture::setUp() {
    // Minimal set up necessary for ServiceEntryPoint.
    auto service = getGlobalServiceContext();

    // Use mock clock source and tick source to test opCtx deadline timings.
    getGlobalServiceContext()->setFastClockSource(std::make_unique<ClockSourceMock>());
    service->setTickSource(std::make_unique<TickSourceMock<>>());

    ReadWriteConcernDefaults::create(service, _lookupMock.getFetchDefaultsFn());
    _lookupMock.setLookupCallReturnValue({});
}

BSONObj ServiceEntryPointTestFixture::dbResponseToBSON(DbResponse& dbResponse) {
    return OpMsg::parse(dbResponse.response).body;
}

void ServiceEntryPointTestFixture::assertErrorResponseIsExpected(BSONObj response,
                                                                 Status expectedStatus,
                                                                 OperationContext* opCtx) {
    auto status = getStatusFromCommandResult(response);
    ASSERT_EQ(CurOp::get(opCtx)->debug().errInfo, expectedStatus);
    ASSERT_EQ(status, expectedStatus);
}

Message ServiceEntryPointTestFixture::constructMessage(BSONObj cmdBSON,
                                                       OperationContext* opCtx,
                                                       uint32_t flags,
                                                       DatabaseName dbName) {
    const static HostAndPort kTestTargetHost = {HostAndPort("FakeHost", 12345)};
    executor::RemoteCommandRequest request{kTestTargetHost, dbName, cmdBSON, opCtx};
    auto msg = static_cast<OpMsgRequest>(request).serialize();
    OpMsg::replaceFlags(&msg, flags);
    return msg;
}

StatusWith<DbResponse> ServiceEntryPointTestFixture::handleRequest(Message msg,
                                                                   OperationContext* opCtx) {
    startCapturingLogMessages();
    auto swDbResponse = getServiceEntryPoint()->handleRequest(opCtx, msg).getNoThrow();
    stopCapturingLogMessages();
    return swDbResponse;
}

DbResponse ServiceEntryPointTestFixture::runCommandTestWithResponse(BSONObj cmdBSON,
                                                                    OperationContext* opCtx,
                                                                    Status expectedStatus) {
    ServiceContext::UniqueOperationContext uniqueOpCtx;
    if (!opCtx) {
        uniqueOpCtx = makeOperationContext();
        opCtx = uniqueOpCtx.get();
    }

    auto msg = constructMessage(cmdBSON, opCtx);
    auto swDbResponse = handleRequest(msg, opCtx);
    ASSERT_OK(swDbResponse);

    auto dbResponse = swDbResponse.getValue();
    auto response = dbResponseToBSON(dbResponse);
    assertErrorResponseIsExpected(response, expectedStatus, opCtx);
    if (!expectedStatus.isOK()) {
        assertResponseForClusterAndOperationTime(response);
    }

    return swDbResponse.getValue();
}

void ServiceEntryPointTestFixture::assertResponseForClusterAndOperationTime(BSONObj response) {
    ASSERT(response.hasField("$clusterTime")) << ", response: " << response;
    ASSERT(response.hasField("operationTime")) << ", response: " << response;
}

void ServiceEntryPointTestFixture::assertCapturedTextLogsContainSubstr(std::string substr) {
    auto logs = getCapturedTextFormatLogMessages();
    ASSERT(std::count_if(logs.begin(), logs.end(), [&](const auto& log) {
        return log.find(substr) != std::string::npos;
    }));
}

// Check `attr` from the log that matches with `msg`.
void ServiceEntryPointTestFixture::assertAttrFromCapturedBSON(std::string msg, BSONObj check) {
    auto logs = getCapturedBSONFormatLogMessages();
    for (const auto& log : logs) {
        if (log.hasField("msg") && log.getField("msg").String() == msg) {
            if (log.hasField("attr")) {
                ASSERT(!SimpleBSONObjComparator().compare(log.getField("attr").Obj(), check));
                return;
            }
        }
    }
    FAIL(str::stream() << check << " not found in logs");
}

}  // namespace mongo
