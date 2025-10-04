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
#include "mongo/db/commands/db_command_test_fixture.h"
#include "mongo/db/commands/profile_cmd_test_utils.h"
#include "mongo/db/commands/profile_gen.h"
#include "mongo/db/profile_filter_impl.h"
#include "mongo/db/profile_settings.h"

namespace mongo {
namespace {

class ProfileCmdTest : public DBCommandTestFixture {
public:
    void setUp() override {
        DBCommandTestFixture::setUp();

        serverGlobalParams.slowMS.store(kDefaultSlowms);
        serverGlobalParams.sampleRate.store(kDefaultSampleRate);

        // Generate a unique database name for each test so that we start a blank slate.
        dbName = DatabaseName::createDatabaseName_forTest(
            boost::none, str::stream() << "profileCmdTestDb" << PseudoRandom(0).nextInt64());
        dbProfilingNs = NamespaceString::makeSystemDotProfileNamespace(dbName);
    }

    BSONObj runCommand(ProfileCmdRequest req, boost::optional<long> errorCode = boost::none) {
        req.setDbName(dbName);
        return runCommand(req.toBSON(), errorCode);
    }

    BSONObj runCommand(BSONObj cmd, boost::optional<long> errorCode = boost::none) {
        return DBCommandTestFixture::runCommand(cmd, dbName, errorCode);
    }

    ProfileSettings getDbProfileSettings() {
        return DatabaseProfileSettings::get(opCtx->getServiceContext())
            .getDatabaseProfileSettings(dbName);
    }

    // Ensure that the correct server state was updated per the profile request.
    void lookupAndValidateProfileSettings(const ProfileCmdTestArgs& req) {
        validateProfileSettings(req, getDbProfileSettings());
    }

    bool dbSystemProfileCollectionExists() {
        return CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, dbProfilingNs) !=
            nullptr;
    }

    DatabaseName dbName;
    NamespaceString dbProfilingNs;
};

TEST_F(ProfileCmdTest, SetProfilingLevel) {
    ProfileCmdTestArgs args{.level = 1};

    // Set the profiling level only. No other state should be touched.
    auto resp = runCommand(buildCmdRequest({.level = 1}));
    validateCmdResponse(resp, {});
    lookupAndValidateProfileSettings(args);

    // 2 is also a valid level.
    args.level = 2;
    resp = runCommand(buildCmdRequest({.level = 2}));
    validateCmdResponse(resp, {.level = 1});
    lookupAndValidateProfileSettings(args);

    // Since the profiling level is greater than zero, the db.system.profile collection should be
    // created.
    ASSERT_TRUE(dbSystemProfileCollectionExists());
}

TEST_F(ProfileCmdTest, ProfilingLevelZeroDoesNotCreateProfileCollection) {
    ProfileCmdTestArgs args{.level = 0, .slowms = 500};

    // Run a command that changes something, but doesn't raise the profiling level above 0.
    auto resp = runCommand(buildCmdRequest(args));
    validateCmdResponse(resp, {});
    lookupAndValidateProfileSettings(args);

    // The db.system.profile collection should not exist since we only changed 'slowms'.
    ASSERT_FALSE(dbSystemProfileCollectionExists());
}

TEST_F(ProfileCmdTest, SetAllParameters) {
    ProfileCmdTestArgs args{.level = 1,
                            .sampleRate = 0.5,
                            .slowms = -1,
                            .filter = ObjectOrUnset{BSON("nreturned" << BSON("$eq" << 1))}};

    auto resp = runCommand(buildCmdRequest(args));
    validateCmdResponse(resp, {});
    lookupAndValidateProfileSettings(args);

    // Since the profiling level is greater than zero, the db.system.profile collection should be
    // created.
    ASSERT_TRUE(dbSystemProfileCollectionExists());

    // Run it a second time as a no-op change to read the settings and ensure they were actually
    // updated (and correctly returned to us).
    resp = runCommand(buildCmdRequest({.level = 1}));
    validateCmdResponse(resp, args);
    lookupAndValidateProfileSettings(args);
}

TEST_F(ProfileCmdTest, ReadOnlyMode) {
    // "Read-only" mode is activated when the profiling level is < 0 or > 2.
    auto resp = runCommand(buildCmdRequest({.level = -1}));
    validateCmdResponse(resp, {});
    lookupAndValidateProfileSettings({});

    resp = runCommand(buildCmdRequest({.level = 3}));
    validateCmdResponse(resp, {});
    lookupAndValidateProfileSettings({});

    // Neither request should have created the db.system.profile collection.
    ASSERT_FALSE(dbSystemProfileCollectionExists());
}

TEST_F(ProfileCmdTest, FilterUnset) {
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

TEST_F(ProfileCmdTest, InvalidFilter) {
    // Invalid input string (i.e., not "unset").
    BSONObj req = fromjson(R"({
        profile: 0,
        filter: "bad input string"
    })");

    auto resp = runCommand(req, ErrorCodes::BadValue);
    ASSERT_EQ(resp["errmsg"].String(), "Expected an object, or the string 'unset'.") << resp;

    // Invalid filter expression.
    req = fromjson(R"({
        profile: 0,
        filter: {nreturned: {$invalidOperator: 1}}
    })");

    resp = runCommand(req, ErrorCodes::BadValue);
    ASSERT_EQ(resp["errmsg"].String(), "unknown operator: $invalidOperator") << resp;
}

TEST_F(ProfileCmdTest, InvalidSampleRate) {
    auto resp = runCommand(buildCmdRequest({.sampleRate = -0.1}), ErrorCodes::BadValue);
    ASSERT_EQ(resp["errmsg"].String(), "'sampleRate' must be between 0.0 and 1.0 inclusive")
        << resp;

    resp = runCommand(buildCmdRequest({.sampleRate = 1.1}), ErrorCodes::BadValue);
    ASSERT_EQ(resp["errmsg"].String(), "'sampleRate' must be between 0.0 and 1.0 inclusive")
        << resp;
}

}  // namespace
}  // namespace mongo
