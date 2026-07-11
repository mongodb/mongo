// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/db/commands/db_command_test_fixture.h"
#include "mongo/db/database_name.h"
#include "mongo/db/query/query_settings/query_settings_service.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

namespace mongo::query_settings {
namespace {

// Extends DBCommandTestFixture (which provides runCommand via DBDirectClient) and re-configures
// the replication coordinator with a replica set name so setQuerySettings passes the standalone
// check. QuerySettingsService is also initialized for the test.
class QuerySettingsCmdsCommandTestFixture : public DBCommandTestFixture {
public:
    void setUp() override {
        DBCommandTestFixture::setUp();
        repl::ReplSettings replSettings;
        replSettings.setReplSetString("rs0");
        auto replCoord =
            std::make_unique<repl::ReplicationCoordinatorMock>(getServiceContext(), replSettings);
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
        replCoord->setCanAcceptNonLocalWrites(true);
        repl::ReplicationCoordinator::set(getServiceContext(), std::move(replCoord));
        QuerySettingsService::initializeForTest(getServiceContext());
    }
};

TEST_F(QuerySettingsCmdsCommandTestFixture,
       SetQuerySettingsRejectsQueryKnobsWhenFeatureFlagDisabled) {
    unittest::ServerParameterGuard featureFlagCtrl("featureFlagPqsQueryKnobs", false);
    runCommand(BSON("setQuerySettings"
                    << BSON("find" << "testColl" << "$db" << "testDb" << "filter" << BSONObj())
                    << "settings" << BSON("queryKnobs" << BSON("testIntKnobWire" << 99))),
               DatabaseName::kAdmin,
               12324800 /*errorCode*/);
}

TEST_F(QuerySettingsCmdsCommandTestFixture,
       SetQuerySettingsRejectsMaxTimeMSWhenFeatureFlagDisabled) {
    unittest::ServerParameterGuard featureFlagCtrl("featureFlagPqsMaxTimeMS", false);
    runCommand(BSON("setQuerySettings"
                    << BSON("find" << "testColl" << "$db" << "testDb" << "filter" << BSONObj())
                    << "settings" << BSON("maxTimeMS" << 5000)),
               DatabaseName::kAdmin,
               12998200 /*errorCode*/);
}

}  // namespace
}  // namespace mongo::query_settings
