/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/s/catalog_cache.h"
#include "mongo/s/database_version.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(ComparableDatabaseVersionTest, VersionsEqual) {
    const DatabaseVersion v(UUID::gen(), Timestamp(1));
    const auto version = ComparableDatabaseVersion::makeComparableDatabaseVersion(v);
    ASSERT(version == version);
}

TEST(ComparableDatabaseVersionTest, VersionsEqualAfterCopy) {
    const DatabaseVersion v(UUID::gen(), Timestamp(1));
    const auto version1 = ComparableDatabaseVersion::makeComparableDatabaseVersion(v);
    const auto version2 = version1;
    ASSERT(version1 == version2);
}

TEST(ComparableDatabaseVersionTest, CompareVersionDifferentTimestamps) {
    const auto version1 = ComparableDatabaseVersion::makeComparableDatabaseVersion(
        DatabaseVersion(UUID::gen(), Timestamp(3)));
    const auto version2 = ComparableDatabaseVersion::makeComparableDatabaseVersion(
        DatabaseVersion(UUID::gen(), Timestamp(2)));
    ASSERT(version2 != version1);
    ASSERT(version2 < version1);
    ASSERT_FALSE(version2 > version1);
}

TEST(ComparableDatabaseVersionTest, VersionGreaterSameTimestamp) {
    const DatabaseVersion v1(UUID::gen(), Timestamp(1));
    const DatabaseVersion v2 = v1.makeUpdated();
    const auto version1 = ComparableDatabaseVersion::makeComparableDatabaseVersion(v1);
    const auto version2 = ComparableDatabaseVersion::makeComparableDatabaseVersion(v2);
    ASSERT(version2 != version1);
    ASSERT(version2 > version1);
    ASSERT_FALSE(version2 < version1);
}

TEST(ComparableDatabaseVersionTest, VersionLessSameTimestamp) {
    const DatabaseVersion v1(UUID::gen(), Timestamp(1));
    const DatabaseVersion v2 = v1.makeUpdated();
    const auto version1 = ComparableDatabaseVersion::makeComparableDatabaseVersion(v1);
    const auto version2 = ComparableDatabaseVersion::makeComparableDatabaseVersion(v2);
    ASSERT(version1 != version2);
    ASSERT(version1 < version2);
    ASSERT_FALSE(version1 > version2);
}

TEST(ComparableDatabaseVersionTest, DefaultConstructedVersionsAreEqual) {
    const ComparableDatabaseVersion defaultVersion1{}, defaultVersion2{};
    ASSERT(defaultVersion1 == defaultVersion2);
    ASSERT_FALSE(defaultVersion1 < defaultVersion2);
    ASSERT_FALSE(defaultVersion1 > defaultVersion2);
}

TEST(ComparableDatabaseVersionTest, DefaultConstructedVersionIsAlwaysLess) {
    DatabaseVersion v(UUID::gen(), Timestamp(1, 1));
    const auto version1 = ComparableDatabaseVersion::makeComparableDatabaseVersion(v);
    const ComparableDatabaseVersion defaultVersion{};
    ASSERT(defaultVersion != version1);
    ASSERT(defaultVersion < version1);
    ASSERT_FALSE(defaultVersion > version1);
}

TEST(ComparableDatabaseVersionTest, CompareForcedRefreshVersionVersusValidDatabaseVersion) {
    const DatabaseVersion v(UUID::gen(), Timestamp(1, 1));
    const ComparableDatabaseVersion defaultVersionBeforeForce;
    const auto versionBeforeForce = ComparableDatabaseVersion::makeComparableDatabaseVersion(v);
    const auto forcedRefreshVersion =
        ComparableDatabaseVersion::makeComparableDatabaseVersionForForcedRefresh();
    const auto versionAfterForce = ComparableDatabaseVersion::makeComparableDatabaseVersion(v);
    const ComparableDatabaseVersion defaultVersionAfterForce;

    ASSERT(defaultVersionBeforeForce != forcedRefreshVersion);
    ASSERT(defaultVersionBeforeForce < forcedRefreshVersion);

    ASSERT(versionBeforeForce != forcedRefreshVersion);
    ASSERT(versionBeforeForce < forcedRefreshVersion);

    ASSERT(versionAfterForce != forcedRefreshVersion);
    ASSERT(versionAfterForce > forcedRefreshVersion);

    ASSERT(defaultVersionAfterForce != forcedRefreshVersion);
    ASSERT(defaultVersionAfterForce < forcedRefreshVersion);
}

TEST(ComparableDatabaseVersionTest, CompareTwoForcedRefreshVersions) {
    const auto forcedRefreshVersion1 =
        ComparableDatabaseVersion::makeComparableDatabaseVersionForForcedRefresh();
    ASSERT(forcedRefreshVersion1 == forcedRefreshVersion1);
    ASSERT_FALSE(forcedRefreshVersion1 < forcedRefreshVersion1);
    ASSERT_FALSE(forcedRefreshVersion1 > forcedRefreshVersion1);

    const auto forcedRefreshVersion2 =
        ComparableDatabaseVersion::makeComparableDatabaseVersionForForcedRefresh();
    ASSERT_FALSE(forcedRefreshVersion1 == forcedRefreshVersion2);
    ASSERT(forcedRefreshVersion1 < forcedRefreshVersion2);
    ASSERT_FALSE(forcedRefreshVersion1 > forcedRefreshVersion2);
}

TEST(ComparableDatabaseVersionTest, CompareVersionsAgainstBoostNone) {
    auto checkGreatherThan = [](const boost::optional<DatabaseVersion>& v1,
                                const boost::optional<DatabaseVersion>& v2) {
        const auto version1 = ComparableDatabaseVersion::makeComparableDatabaseVersion(v1);
        const auto version2 = ComparableDatabaseVersion::makeComparableDatabaseVersion(v2);
        ASSERT_TRUE(version1 < version2);
    };
    const DatabaseVersion v(UUID::gen(), Timestamp(42));
    checkGreatherThan(boost::none, v);
    checkGreatherThan(v, boost::none);
    checkGreatherThan(boost::none, boost::none);
}

}  // namespace
}  // namespace mongo
