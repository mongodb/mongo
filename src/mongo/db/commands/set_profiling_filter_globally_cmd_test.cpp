// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
            .setDatabaseProfileSettings(dbName, {0, std::move(profileFilter), Milliseconds(0)});
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
