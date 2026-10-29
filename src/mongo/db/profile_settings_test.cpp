// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/profile_settings.h"

#include "mongo/unittest/unittest.h"

#include <string_view>

namespace mongo {
namespace {

/**
 * A test fixture that creates a CollectionCatalog and const CollectionPtr& pointer to store in it.
 */
class ProfileSettingsTest : public unittest::Test {
public:
    ProfileSettingsTest() {}

protected:
    DatabaseProfileSettings dbProfileSettings;
};

class ProfileFilterMock : public ProfileFilter {
public:
    ProfileFilterMock(int i) : id(i) {}

    bool matches(OperationContext*, const OpDebug&, const CurOp&) const final {
        return true;
    }
    BSONObj serialize() const final {
        return BSONObj();
    }

    bool dependsOn(std::string_view topLevelField) const final {
        return true;
    };

    int id;
};

// Test setting and fetching the profile level for a database.
TEST_F(ProfileSettingsTest, DefaultProfileLevel) {
    DatabaseName testDBNameFirst =
        DatabaseName::createDatabaseName_forTest(boost::none, "testdbfirst");
    DatabaseName testDBNameSecond =
        DatabaseName::createDatabaseName_forTest(boost::none, "testdbsecond");

    dbProfileSettings.setDefaultLevel(1);

    // Requesting a profile level that is not in the _databaseProfileLevel map should return the
    // default server-wide setting
    ASSERT_EQ(dbProfileSettings.getDatabaseProfileSettings(testDBNameFirst).level, 1);

    // Setting the default profile level should have not changed the result.
    dbProfileSettings.setDatabaseProfileSettings(testDBNameFirst, {1, nullptr, Milliseconds(5000)});
    ASSERT_EQ(dbProfileSettings.getDatabaseProfileSettings(testDBNameFirst).level, 1);

    // Changing the profile level should make fetching it different.
    dbProfileSettings.setDatabaseProfileSettings(testDBNameSecond,
                                                 {2, nullptr, Milliseconds(5000)});
    ASSERT_EQ(dbProfileSettings.getDatabaseProfileSettings(testDBNameSecond).level, 2);
}

TEST_F(ProfileSettingsTest, DefaultDatabaseFilter) {
    DatabaseName testDBNameFirst =
        DatabaseName::createDatabaseName_forTest(boost::none, "testdbfirst");
    DatabaseName testDBNameSecond =
        DatabaseName::createDatabaseName_forTest(boost::none, "testdbsecond");

    auto defaultFilter = std::make_shared<ProfileFilterMock>(1);
    dbProfileSettings.setDefaultFilter(defaultFilter);

    // Return the default filter when one has not been explicitly set.
    auto filter = dynamic_cast<const ProfileFilterMock*>(
        dbProfileSettings.getDatabaseProfileSettings(testDBNameFirst).filter.get());
    ASSERT(filter);
    ASSERT_EQ(filter->id, defaultFilter->id);

    // Setting a specific filter should override the default .
    auto specificFilter = std::make_shared<ProfileFilterMock>(2);
    dbProfileSettings.setDatabaseProfileSettings(testDBNameSecond,
                                                 {1, specificFilter, Milliseconds(5000)});
    filter = dynamic_cast<const ProfileFilterMock*>(
        dbProfileSettings.getDatabaseProfileSettings(testDBNameSecond).filter.get());
    ASSERT(filter);
    ASSERT_EQ(filter->id, specificFilter->id);
}

TEST_F(ProfileSettingsTest, DefaultSlowOpInProgressThreshold) {
    DatabaseName testDBNameFirst =
        DatabaseName::createDatabaseName_forTest(boost::none, "testdbfirst");
    DatabaseName testDBNameSecond =
        DatabaseName::createDatabaseName_forTest(boost::none, "testdbsecond");

    auto defaultSlowOpInProgressThreshold = Milliseconds(100);
    dbProfileSettings.setDefaultSlowOpInProgressThreshold(defaultSlowOpInProgressThreshold);

    // Return the default filter when one has not been explicitly set.
    auto slowOpInProgressThreshold =
        dbProfileSettings.getDatabaseProfileSettings(testDBNameFirst).slowOpInProgressThreshold;
    ASSERT_EQ(slowOpInProgressThreshold, defaultSlowOpInProgressThreshold);

    // Setting a specific filter should override the default .
    auto specificSlowOpThreshold = Milliseconds(200);
    dbProfileSettings.setDatabaseProfileSettings(testDBNameSecond,
                                                 {1, nullptr, specificSlowOpThreshold});
    slowOpInProgressThreshold =
        dbProfileSettings.getDatabaseProfileSettings(testDBNameSecond).slowOpInProgressThreshold;
    ASSERT_EQ(slowOpInProgressThreshold, specificSlowOpThreshold);
}

TEST_F(ProfileSettingsTest, SetAll) {
    DatabaseName testDBNameFirst =
        DatabaseName::createDatabaseName_forTest(boost::none, "testdbfirst");
    DatabaseName testDBNameSecond =
        DatabaseName::createDatabaseName_forTest(boost::none, "testdbsecond");

    // Set a filter on one database. This should be overridden when setting a filter on all
    // databases.
    auto specificFilter = std::make_shared<ProfileFilterMock>(2);
    dbProfileSettings.setDatabaseProfileSettings(testDBNameSecond,
                                                 {2, specificFilter, Milliseconds(5000)});

    // Setting a profile filter on all databases and changing the default applies to all databases.
    auto defaultFilter = std::make_shared<ProfileFilterMock>(1);
    dbProfileSettings.setAllDatabaseProfileFiltersAndDefault(defaultFilter);
    auto filter = dynamic_cast<const ProfileFilterMock*>(
        dbProfileSettings.getDatabaseProfileSettings(testDBNameFirst).filter.get());
    ASSERT(filter);
    ASSERT_EQ(filter->id, defaultFilter->id);

    filter = dynamic_cast<const ProfileFilterMock*>(
        dbProfileSettings.getDatabaseProfileSettings(testDBNameSecond).filter.get());
    ASSERT(filter);
    ASSERT_EQ(filter->id, defaultFilter->id);
}
}  // namespace
}  // namespace mongo
