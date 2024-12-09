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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/commands/profile_cmd_test_utils.h"
#include "mongo/db/commands/profile_gen.h"
#include "mongo/db/profile_filter_impl.h"
#include "mongo/db/profile_settings.h"
#include "mongo/rpc/factory.h"
#include "mongo/s/commands/cluster_command_test_fixture.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

class ClusterProfileCmdTest : public ClusterCommandTestFixture {
public:
    // Profile commands should never be sent on to the shards, so don't provide an implementation
    // for these functions.
    void expectInspectRequest(int shardIndex, InspectionCallback cb) override {
        MONGO_UNREACHABLE;
    }
    void expectReturnsSuccess(int shardIndex) override {
        MONGO_UNREACHABLE;
    }

    void setUp() override {
        ClusterCommandTestFixture::setUp();

        serverGlobalParams.slowMS.store(kDefaultSlowms);
        serverGlobalParams.sampleRate.store(kDefaultSampleRate);
    }

    ProfileSettings getDbProfileSettings() {
        return DatabaseProfileSettings::get(getServiceContext())
            .getDatabaseProfileSettings(kNss.dbName());
    }

    // Ensure that the correct server state was updated per the profile request.
    void lookupAndValidateProfileSettings(const ProfileCmdTestArgs& req) {
        validateProfileSettings(req, getDbProfileSettings());
    }

    BSONObj runCommand(ProfileCmdRequest req, boost::optional<long> errorCode = boost::none) {
        req.setDbName(kNss.dbName());

        auto cmd = req.toBSON();
        auto dbResponse = ClusterCommandTestFixture::runCommand(cmd);
        auto result = rpc::makeReply(&dbResponse.response)->getCommandReply().getOwned();

        if (errorCode) {
            ASSERT_TRUE(result.hasField("code") && result["code"].isNumber()) << result;
            ASSERT_EQ(result["code"].Number(), *errorCode) << result;
        } else {
            ASSERT_TRUE(result.hasField("ok") && result["ok"].isNumber() &&
                        result["ok"].Number() == 1)
                << result;
        }

        return result;
    }
};

TEST_F(ClusterProfileCmdTest, EnableProfilingFails) {
    // Set the profiling level to greater than 0 (not allowed on mongos).
    auto resp = runCommand(buildCmdRequest({.level = 1}), ErrorCodes::BadValue);
    ASSERT_TRUE(resp["errmsg"].String().starts_with("Profiling is not permitted on mongoS"))
        << resp;

    // Level 2 is not valid either.
    resp = runCommand(buildCmdRequest({.level = 2}), ErrorCodes::BadValue);
    ASSERT_TRUE(resp["errmsg"].String().starts_with("Profiling is not permitted on mongoS"))
        << resp;

    // No settings should have changed.
    lookupAndValidateProfileSettings({.level = 0});
}

TEST_F(ClusterProfileCmdTest, SetAllParameters) {
    ProfileCmdTestArgs args{.level = 0,
                            .sampleRate = 0.5,
                            .slowms = -1,
                            .filter = ObjectOrUnset{BSON("nreturned" << BSON("$eq" << 1))}};

    auto resp = runCommand(buildCmdRequest(args));
    validateCmdResponse(resp, {});
    lookupAndValidateProfileSettings(args);

    // Run it a second time as a no-op change to read the settings and ensure they were actually
    // updated (and correctly returned to us).
    resp = runCommand(buildCmdRequest({.level = 0}));
    validateCmdResponse(resp, args);
    lookupAndValidateProfileSettings(args);
}

TEST_F(ClusterProfileCmdTest, FilterUnset) {
    // First set the filter to something non-null.
    ProfileCmdTestArgs args{.filter = ObjectOrUnset{BSON("nreturned" << BSON("$eq" << 1))}};

    auto resp = runCommand(buildCmdRequest(args));
    validateCmdResponse(resp, {});
    lookupAndValidateProfileSettings(args);

    // Now unset the filter.
    ProfileCmdTestArgs unsetArgs{.filter = ObjectOrUnset{}};
    resp = runCommand(buildCmdRequest(unsetArgs));
    validateCmdResponse(resp, args);
    lookupAndValidateProfileSettings(unsetArgs);
}


}  // namespace
}  // namespace mongo
