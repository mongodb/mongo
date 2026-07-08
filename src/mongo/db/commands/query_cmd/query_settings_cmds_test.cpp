/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
