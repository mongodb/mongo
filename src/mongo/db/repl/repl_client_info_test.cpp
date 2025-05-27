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

#include "mongo/db/repl/repl_client_info.h"

#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace repl {
namespace {

class ReplClientInfoTest : public ServiceContextMongoDTest {
private:
    void setUp() override;
};

void ReplClientInfoTest::setUp() {
    // Set up mongod.
    ServiceContextMongoDTest::setUp();

    auto service = getServiceContext();
    auto opCtx = cc().makeOperationContext();

    // Set up ReplicationCoordinator.
    ReplicationCoordinator::set(service, std::make_unique<ReplicationCoordinatorMock>(service));

    // Ensure that we are primary.
    auto replCoord = ReplicationCoordinator::get(opCtx.get());
    ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_PRIMARY));
}

TEST_F(ReplClientInfoTest, SetLastOpToSystemLastOpTimeFallsBackToMyLastWrittenOptime) {
    auto opCtx = cc().makeOperationContext();

    ReplClientInfo::forClient(&cc()).setLastOpToSystemLastOpTime(opCtx.get());
    ASSERT_TRUE(ReplClientInfo::forClient(opCtx->getClient())
                    .lastOpWasSetExplicitlyByClientForCurrentOperation(opCtx.get()));
    auto lastOp = ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    auto replCoord = ReplicationCoordinator::get(opCtx.get());
    ASSERT_EQUALS(replCoord->getMyLastWrittenOpTime(), lastOp);
}

}  // namespace
}  // namespace repl
}  // namespace mongo
