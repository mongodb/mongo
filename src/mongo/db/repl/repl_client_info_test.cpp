// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
