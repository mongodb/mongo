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

#include "mongo/db/profile_settings.h"

#include "mongo/unittest/unittest.h"

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

    bool dependsOn(StringData topLevelField) const final {
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
    dbProfileSettings.setDatabaseProfileSettings(testDBNameFirst, {1, nullptr});
    ASSERT_EQ(dbProfileSettings.getDatabaseProfileSettings(testDBNameFirst).level, 1);

    // Changing the profile level should make fetching it different.
    dbProfileSettings.setDatabaseProfileSettings(testDBNameSecond, {2, nullptr});
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
    dbProfileSettings.setDatabaseProfileSettings(testDBNameSecond, {1, specificFilter});
    filter = dynamic_cast<const ProfileFilterMock*>(
        dbProfileSettings.getDatabaseProfileSettings(testDBNameSecond).filter.get());
    ASSERT(filter);
    ASSERT_EQ(filter->id, specificFilter->id);
}


TEST_F(ProfileSettingsTest, SetAll) {
    DatabaseName testDBNameFirst =
        DatabaseName::createDatabaseName_forTest(boost::none, "testdbfirst");
    DatabaseName testDBNameSecond =
        DatabaseName::createDatabaseName_forTest(boost::none, "testdbsecond");

    // Set a filter on one database. This should be overridden when setting a filter on all
    // databases.
    auto specificFilter = std::make_shared<ProfileFilterMock>(2);
    dbProfileSettings.setDatabaseProfileSettings(testDBNameSecond, {2, specificFilter});

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
