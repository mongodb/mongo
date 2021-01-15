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
    auto versionsEqual = [](const DatabaseVersion& v) {
        const auto version = ComparableDatabaseVersion::makeComparableDatabaseVersion(v);
        ASSERT(version.getVersion() == v);
        ASSERT(version == version);
    };

    versionsEqual(DatabaseVersion(UUID::gen()));
    versionsEqual(DatabaseVersion(UUID::gen(), Timestamp(1)));
}

TEST(ComparableDatabaseVersionTest, VersionsEqualAfterCopy) {
    auto versionsEqualAfterCopy = [](const DatabaseVersion& v) {
        const auto version1 = ComparableDatabaseVersion::makeComparableDatabaseVersion(v);
        const auto version2 = version1;
        ASSERT(version1 == version2);
    };

    versionsEqualAfterCopy(DatabaseVersion(UUID::gen()));
    versionsEqualAfterCopy(DatabaseVersion(UUID::gen(), Timestamp(1)));
}

TEST(ComparableDatabaseVersionTest, CompareVersionDifferentUuids) {
    const auto version1 =
        ComparableDatabaseVersion::makeComparableDatabaseVersion(DatabaseVersion(UUID::gen()));
    const auto version2 =
        ComparableDatabaseVersion::makeComparableDatabaseVersion(DatabaseVersion(UUID::gen()));
    ASSERT(version2 != version1);
    ASSERT(version2 > version1);
    ASSERT_FALSE(version2 < version1);
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


TEST(ComparableDatabaseVersionTest, CompareEpochBasedVersionAgainstEpochAndTimestampBasedVersion) {
    {
        auto equalVersions = [](const DatabaseVersion& v1, const DatabaseVersion& v2) {
            const auto version1 = ComparableDatabaseVersion::makeComparableDatabaseVersion(v1);
            const auto version2 = ComparableDatabaseVersion::makeComparableDatabaseVersion(v2);
            ASSERT(version1 == version2);
            ASSERT_FALSE(version1 < version2);
            ASSERT_FALSE(version1 > version2);
        };

        const auto epoch = UUID::gen();
        const DatabaseVersion v1(epoch);
        const DatabaseVersion v2(epoch, Timestamp(1));
        equalVersions(v1, v2);
        equalVersions(v2, v1);
    }

    {
        auto diffVersionsMoreRecentByLastMod = [](const DatabaseVersion& v1,
                                                  const DatabaseVersion& v2) {
            const auto version1 = ComparableDatabaseVersion::makeComparableDatabaseVersion(v1);
            const auto version2 = ComparableDatabaseVersion::makeComparableDatabaseVersion(v2);
            ASSERT(version1 != version2);
            ASSERT(version1 > version2);
            ASSERT_FALSE(version1 < version2);
        };

        const auto epoch = UUID::gen();
        const DatabaseVersion v1(epoch);
        const DatabaseVersion v2(epoch, Timestamp(1));
        diffVersionsMoreRecentByLastMod(v1.makeUpdated(), v2);
        diffVersionsMoreRecentByLastMod(v2.makeUpdated(), v1);
    }

    {
        auto diffVersionsMoreRecentByDisambigSeqNum = [](const DatabaseVersion& v1,
                                                         const DatabaseVersion& v2) {
            const auto version1 = ComparableDatabaseVersion::makeComparableDatabaseVersion(v1);
            const auto version2 = ComparableDatabaseVersion::makeComparableDatabaseVersion(v2);
            ASSERT(version1 != version2);
            ASSERT(version1 < version2);
            ASSERT_FALSE(version1 > version2);
        };

        const DatabaseVersion v1(UUID::gen());
        const DatabaseVersion v2(UUID::gen(), Timestamp(1));
        diffVersionsMoreRecentByDisambigSeqNum(v1, v2);
        diffVersionsMoreRecentByDisambigSeqNum(v2, v1);
    }
}

TEST(ComparableDatabaseVersionTest, VersionGreaterSameUuidOrTimestamp) {
    auto versionGreaterSameUuidOrTimestamp = [](const DatabaseVersion& v1) {
        const DatabaseVersion v2 = v1.makeUpdated();
        const auto version1 = ComparableDatabaseVersion::makeComparableDatabaseVersion(v1);
        const auto version2 = ComparableDatabaseVersion::makeComparableDatabaseVersion(v2);
        ASSERT(version2 != version1);
        ASSERT(version2 > version1);
        ASSERT_FALSE(version2 < version1);
    };

    versionGreaterSameUuidOrTimestamp(DatabaseVersion(UUID::gen()));
    versionGreaterSameUuidOrTimestamp(DatabaseVersion(UUID::gen(), Timestamp(1)));
}

TEST(ComparableDatabaseVersionTest, VersionLessSameUuidOrTimestamp) {
    auto versionLessSameUuidOrTimestamp = [](const DatabaseVersion& v1) {
        const DatabaseVersion v2 = v1.makeUpdated();
        const auto version1 = ComparableDatabaseVersion::makeComparableDatabaseVersion(v1);
        const auto version2 = ComparableDatabaseVersion::makeComparableDatabaseVersion(v2);
        ASSERT(version1 != version2);
        ASSERT(version1 < version2);
        ASSERT_FALSE(version1 > version2);
    };

    versionLessSameUuidOrTimestamp(DatabaseVersion(UUID::gen()));
    versionLessSameUuidOrTimestamp(DatabaseVersion(UUID::gen(), Timestamp(1)));
}

TEST(ComparableDatabaseVersionTest, DefaultConstructedVersionsAreEqual) {
    const ComparableDatabaseVersion defaultVersion1{}, defaultVersion2{};
    ASSERT(defaultVersion1 == defaultVersion2);
    ASSERT_FALSE(defaultVersion1 < defaultVersion2);
    ASSERT_FALSE(defaultVersion1 > defaultVersion2);
}

TEST(ComparableDatabaseVersionTest, DefaultConstructedVersionIsAlwaysLess) {

    auto defaultConstructedVersionIsAlwaysLess = [](const DatabaseVersion& v) {
        const ComparableDatabaseVersion defaultVersion{};
        const auto version1 = ComparableDatabaseVersion::makeComparableDatabaseVersion(v);
        ASSERT(defaultVersion != version1);
        ASSERT(defaultVersion < version1);
        ASSERT_FALSE(defaultVersion > version1);
    };

    defaultConstructedVersionIsAlwaysLess(DatabaseVersion(UUID::gen()));
    defaultConstructedVersionIsAlwaysLess(DatabaseVersion(UUID::gen(), Timestamp(1)));
}

}  // namespace
}  // namespace mongo
