// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/timestamp.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

#include <memory>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

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
