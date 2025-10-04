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

#include "mongo/db/commands/db_command_test_fixture.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/profile_filter_impl.h"
#include "mongo/db/profile_settings.h"

namespace mongo {
namespace {

class SetProfilingFilterGloballyCmdTest : public DBCommandTestFixture {
public:
    void setUp() override {
        DBCommandTestFixture::setUp();

        internalQueryGlobalProfilingFilter.store(true);
        setDefaultFilter(boost::none);
    }

    BSONObj runCommandWithFilter(boost::optional<BSONObj> filterObj) {
        auto cmd = filterObj ? BSON("setProfilingFilterGlobally" << 1 << "filter" << *filterObj)
                             : BSON("setProfilingFilterGlobally" << 1 << "filter"
                                                                 << "unset");
        return runCommand(cmd);
    }

    void addDatabase() {
        databases.push_back(DatabaseName::createDatabaseName_forTest(
            boost::none, str::stream() << "set_profiling_filter_globally" << databases.size()));
    }

    void setDefaultFilter(boost::optional<BSONObj> filter) {
        DatabaseProfileSettings::get(opCtx->getServiceContext())
            .setDefaultFilter(filter ? std::make_shared<ProfileFilterImpl>(
                                           *filter, ExpressionContextBuilder{}.opCtx(opCtx).build())
                                     : std::shared_ptr<ProfileFilterImpl>(nullptr));
    }

    void checkDefaultFilter(boost::optional<BSONObj> filterObj) {
        auto defaultFilter =
            DatabaseProfileSettings::get(opCtx->getServiceContext()).getDefaultFilter();
        if (!filterObj) {
            ASSERT_EQ(defaultFilter, nullptr);
        } else {
            ASSERT_BSONOBJ_EQ(defaultFilter->serialize(), *filterObj);
        }
    }

    boost::optional<BSONObj> getDatabaseProfileFilter(const DatabaseName& dbName) {
        auto settings = DatabaseProfileSettings::get(opCtx->getServiceContext())
                            .getDatabaseProfileSettings(dbName);
        return settings.filter ? boost::optional<BSONObj>(settings.filter->serialize())
                               : boost::none;
    }

    void setDatabaseProfileFilter(const DatabaseName& dbName, boost::optional<BSONObj> filter) {
        std::shared_ptr<ProfileFilter> profileFilter = filter
            ? std::make_shared<ProfileFilterImpl>(*filter,
                                                  ExpressionContextBuilder{}.opCtx(opCtx).build())
            : std::shared_ptr<ProfileFilterImpl>(nullptr);
        DatabaseProfileSettings::get(opCtx->getServiceContext())
            .setDatabaseProfileSettings(dbName, {0, std::move(profileFilter)});
    }

    std::vector<DatabaseName> databases;
};

TEST_F(SetProfilingFilterGloballyCmdTest, SetFilterNoDatabasesPresent) {
    auto filterToSet = BSON("nreturned" << BSON("$eq" << 5));

    auto result = runCommandWithFilter(filterToSet);
    ASSERT(result.hasField("was"));
    ASSERT_EQ(result.getStringField("was"), "none");

    checkDefaultFilter(filterToSet);
}

TEST_F(SetProfilingFilterGloballyCmdTest, SetFilterMultipleDatabases) {
    auto filterToSet = BSON("nreturned" << BSON("$eq" << 5));

    addDatabase();
    addDatabase();

    auto result = runCommandWithFilter(filterToSet);
    ASSERT(result.hasField("was"));
    ASSERT_EQ(result.getStringField("was"), "none");

    // The profile filter should also apply to databases added after the command runs.
    addDatabase();

    checkDefaultFilter(filterToSet);

    for (const auto& dbName : databases) {
        auto dbFilter = getDatabaseProfileFilter(dbName);
        ASSERT(dbFilter);
        ASSERT_BSONOBJ_EQ(*dbFilter, filterToSet);
    }
}

TEST_F(SetProfilingFilterGloballyCmdTest, OverwriteFilterMultipleDatabases) {
    auto originalDefault = BSON("nreturned" << BSON("$eq" << 5));
    auto filterToSet = BSON("nreturned" << BSON("$eq" << 20));

    setDefaultFilter(originalDefault);

    addDatabase();
    setDatabaseProfileFilter(databases.back(), BSON("nreturned" << BSON("$eq" << 10)));
    addDatabase();
    setDatabaseProfileFilter(databases.back(), BSON("nreturned" << BSON("$eq" << 15)));
    addDatabase();
    setDatabaseProfileFilter(databases.back(), boost::none);

    auto result = runCommandWithFilter(filterToSet);

    ASSERT(result.hasField("was"));
    ASSERT_BSONOBJ_EQ(result.getObjectField("was"), originalDefault);

    checkDefaultFilter(filterToSet);

    for (const auto& dbName : databases) {
        auto dbFilter = getDatabaseProfileFilter(dbName);
        ASSERT(dbFilter);
        ASSERT_BSONOBJ_EQ(*dbFilter, filterToSet);
    }
}

TEST_F(SetProfilingFilterGloballyCmdTest, UnsetNoFilter) {
    addDatabase();

    auto result = runCommandWithFilter(boost::none);
    ASSERT(result.hasField("was"));
    ASSERT_EQ(result.getStringField("was"), "none");

    checkDefaultFilter(boost::none);

    for (const auto& dbName : databases) {
        ASSERT_EQ(getDatabaseProfileFilter(dbName), boost::none);
    }
}

TEST_F(SetProfilingFilterGloballyCmdTest, UnsetNonEmptyFilter) {
    auto originalDefault = BSON("nreturned" << BSON("$eq" << 5));
    setDefaultFilter(originalDefault);

    addDatabase();
    setDatabaseProfileFilter(databases.back(), BSON("nreturned" << BSON("$eq" << 10)));
    addDatabase();

    auto result = runCommandWithFilter(boost::none);
    ASSERT(result.hasField("was"));
    ASSERT_BSONOBJ_EQ(result.getObjectField("was"), originalDefault);

    checkDefaultFilter(boost::none);

    for (const auto& dbName : databases) {
        ASSERT_EQ(getDatabaseProfileFilter(dbName), boost::none);
    }
}

}  // namespace
}  // namespace mongo
