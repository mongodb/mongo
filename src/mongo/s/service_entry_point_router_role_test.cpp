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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/s/service_entry_point_router_role.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/service_entry_point_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class ServiceEntryPointRouterRoleTest : public virtual service_context_test::RouterRoleOverride,
                                        public ServiceEntryPointTestFixture {
public:
    void setUp() override {
        ServiceEntryPointTestFixture::setUp();
        auto routerService = getGlobalServiceContext()->getService(ClusterRole::RouterServer);
        ReadWriteConcernDefaults::create(routerService, _lookupMock.getFetchDefaultsFn());
        _lookupMock.setLookupCallReturnValue({});

        getGlobalServiceContext()->getService()->setServiceEntryPoint(
            std::make_unique<ServiceEntryPointRouterRole>());
    }
    void assertResponseForClusterAndOperationTime(BSONObj response) override {
        // For ServiceEntryPointRouterRole, either cluster time or operation time will be present,
        // or neither field.
        ASSERT_FALSE(response.hasField("$clusterTime") && response.hasField("operationTime"))
            << ", response: " << response;
    }
    void assertResponseStatus(BSONObj response,
                              Status expectedStatus,
                              OperationContext* opCtx) override {
        auto status = getStatusFromCommandResult(response);
        // TODO SERVER-97138 there are some differences between shard and router SEPs with
        // regard to when errInfo is recorded. Once those are resolved, this function can be
        // a non-virtual member of ServiceEntryPointTestFixture.
        // ASSERT_EQ(CurOp::get(opCtx)->debug().errInfo, expectedStatus);
        ASSERT_EQ(status, expectedStatus);
    }
};

TEST_F(ServiceEntryPointRouterRoleTest, TestCommandSucceeds) {
    testCommandSucceeds();
}

TEST_F(ServiceEntryPointRouterRoleTest, TestCommandFailsRunInvocationWithResponse) {
    testCommandFailsRunInvocationWithResponse();
}

TEST_F(ServiceEntryPointRouterRoleTest, TestCommandFailsRunInvocationWithException) {
    testCommandFailsRunInvocationWithException("Exception thrown while processing command");
}

TEST_F(ServiceEntryPointRouterRoleTest, HandleRequestException) {
    testHandleRequestException(5745706);
}

TEST_F(ServiceEntryPointRouterRoleTest, ParseCommandFailsDbQueryUnsupportedCommand) {
    testParseCommandFailsDbQueryUnsupportedCommand("Exception thrown while parsing command");
}

TEST_F(ServiceEntryPointRouterRoleTest, TestCommandNotFound) {
    testCommandNotFound(false);
}

TEST_F(ServiceEntryPointRouterRoleTest, HelloCmdSetsClientMetadata) {
    testHelloCmdSetsClientMetadata();
}

TEST_F(ServiceEntryPointRouterRoleTest, TestCommentField) {
    testCommentField();
}

TEST_F(ServiceEntryPointRouterRoleTest, TestHelpField) {
    testHelpField();
}

TEST_F(ServiceEntryPointRouterRoleTest, TestCommandServiceCounters) {
    testCommandServiceCounters(ClusterRole::RouterServer);
}

TEST_F(ServiceEntryPointRouterRoleTest, TestCommandMaxTimeMS) {
    testCommandMaxTimeMS();
}

TEST_F(ServiceEntryPointRouterRoleTest, TestOpCtxInterrupt) {
    testOpCtxInterrupt(false);
}

TEST_F(ServiceEntryPointRouterRoleTest, TestReadConcernClientUnspecifiedNoDefault) {
    testReadConcernClientUnspecifiedNoDefault();
}

TEST_F(ServiceEntryPointRouterRoleTest, TestReadConcernClientUnspecifiedWithDefault) {
    testReadConcernClientUnspecifiedWithDefault(true);
}

TEST_F(ServiceEntryPointRouterRoleTest, TestReadConcernClientSuppliedLevelNotAllowed) {
    testReadConcernClientSuppliedLevelNotAllowed(false);
}

TEST_F(ServiceEntryPointRouterRoleTest, TestReadConcernClientSuppliedAllowed) {
    testReadConcernClientSuppliedAllowed();
}

TEST_F(ServiceEntryPointRouterRoleTest, TestReadConcernExtractedOnException) {
    testReadConcernExtractedOnException();
}

TEST_F(ServiceEntryPointRouterRoleTest, TestCommandInvocationHooks) {
    testCommandInvocationHooks();
}

TEST_F(ServiceEntryPointRouterRoleTest, TestExhaustCommandNextInvocationSet) {
    testExhaustCommandNextInvocationSet();
}

TEST_F(ServiceEntryPointRouterRoleTest, TestWriteConcernClientSpecified) {
    testWriteConcernClientSpecified();
}

TEST_F(ServiceEntryPointRouterRoleTest, TestWriteConcernClientUnspecifiedNoDefault) {
    testWriteConcernClientUnspecifiedNoDefault();
}

TEST_F(ServiceEntryPointRouterRoleTest, TestWriteConcernClientUnspecifiedWithDefault) {
    testWriteConcernClientUnspecifiedWithDefault(true);
}

}  // namespace
}  // namespace mongo
