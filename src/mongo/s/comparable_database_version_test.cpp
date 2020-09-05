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
#include "mongo/s/database_version_helpers.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(ComparableDatabaseVersionTest, VersionsEqual) {
    DatabaseVersion dbVersion{UUID::gen(), 1};
    const auto version = ComparableDatabaseVersion::makeComparableDatabaseVersion(dbVersion);
    ASSERT(databaseVersion::equal(version.getVersion(), dbVersion));
    ASSERT(version == version);
}

TEST(ComparableDatabaseVersionTest, VersionsEqualAfterCopy) {
    DatabaseVersion dbVersion{UUID::gen(), 1};
    const auto version1 = ComparableDatabaseVersion::makeComparableDatabaseVersion(dbVersion);
    const auto version2 = version1;
    ASSERT(version1 == version2);
}

TEST(ComparableDatabaseVersionTest, CompareVersionDifferentUuids) {
    const auto version1 =
        ComparableDatabaseVersion::makeComparableDatabaseVersion(DatabaseVersion(UUID::gen(), 2));
    const auto version2 =
        ComparableDatabaseVersion::makeComparableDatabaseVersion(DatabaseVersion(UUID::gen(), 1));
    ASSERT(version2 != version1);
    ASSERT(version2 > version1);
    ASSERT_FALSE(version2 < version1);
}

TEST(ComparableDatabaseVersionTest, VersionGreaterSameUuid) {
    const auto uuid = UUID::gen();
    const auto version1 =
        ComparableDatabaseVersion::makeComparableDatabaseVersion(DatabaseVersion(uuid, 1));
    const auto version2 =
        ComparableDatabaseVersion::makeComparableDatabaseVersion(DatabaseVersion(uuid, 2));
    ASSERT(version2 != version1);
    ASSERT(version2 > version1);
    ASSERT_FALSE(version2 < version1);
}

TEST(ComparableDatabaseVersionTest, VersionLessSameUuid) {
    const auto uuid = UUID::gen();
    const auto version1 =
        ComparableDatabaseVersion::makeComparableDatabaseVersion(DatabaseVersion(uuid, 1));
    const auto version2 =
        ComparableDatabaseVersion::makeComparableDatabaseVersion(DatabaseVersion(uuid, 2));
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
    const ComparableDatabaseVersion defaultVersion{};
    const auto version1 =
        ComparableDatabaseVersion::makeComparableDatabaseVersion(DatabaseVersion(UUID::gen(), 0));
    ASSERT(defaultVersion != version1);
    ASSERT(defaultVersion < version1);
    ASSERT_FALSE(defaultVersion > version1);
}

}  // namespace
}  // namespace mongo
