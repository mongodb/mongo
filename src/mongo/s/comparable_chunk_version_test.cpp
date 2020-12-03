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

#include "mongo/s/chunk_manager.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(ComparableChunkVersionTest, VersionsEqual) {
    auto epoch = OID::gen();
    const auto version1 = ComparableChunkVersion::makeComparableChunkVersion(
        ChunkVersion(1, 0, epoch, boost::none /* timestamp */));
    const auto version2 = ComparableChunkVersion::makeComparableChunkVersion(
        ChunkVersion(1, 0, epoch, boost::none /* timestamp */));
    ASSERT(version1.getVersion() == version2.getVersion());
    ASSERT(version1 == version2);
}

TEST(ComparableChunkVersionTest, VersionsEqualAfterCopy) {
    ChunkVersion chunkVersion(1, 0, OID::gen(), boost::none /* timestamp */);
    const auto version1 = ComparableChunkVersion::makeComparableChunkVersion(chunkVersion);
    const auto version2 = version1;
    ASSERT(version1 == version2);
}

TEST(ComparableChunkVersionTest, CompareVersionDifferentEpochs) {
    const auto version1 = ComparableChunkVersion::makeComparableChunkVersion(
        ChunkVersion(2, 0, OID::gen(), boost::none /* timestamp */));
    const auto version2 = ComparableChunkVersion::makeComparableChunkVersion(
        ChunkVersion(1, 0, OID::gen(), boost::none /* timestamp */));
    ASSERT(version2 != version1);
    ASSERT(version2 > version1);
    ASSERT_FALSE(version2 < version1);
}

TEST(ComparableChunkVersionTest, VersionGreaterSameEpochs) {
    const auto epoch = OID::gen();
    const auto version1 = ComparableChunkVersion::makeComparableChunkVersion(
        ChunkVersion(1, 0, epoch, boost::none /* timestamp */));
    const auto version2 = ComparableChunkVersion::makeComparableChunkVersion(
        ChunkVersion(1, 1, epoch, boost::none /* timestamp */));
    const auto version3 = ComparableChunkVersion::makeComparableChunkVersion(
        ChunkVersion(2, 0, epoch, boost::none /* timestamp */));
    ASSERT(version2 != version1);
    ASSERT(version2 > version1);
    ASSERT_FALSE(version2 < version1);
    ASSERT(version3 != version2);
    ASSERT(version3 > version2);
    ASSERT_FALSE(version3 < version2);
}

TEST(ComparableChunkVersionTest, VersionLessSameEpoch) {
    const auto epoch = OID::gen();
    const auto version1 = ComparableChunkVersion::makeComparableChunkVersion(
        ChunkVersion(1, 0, epoch, boost::none /* timestamp */));
    const auto version2 = ComparableChunkVersion::makeComparableChunkVersion(
        ChunkVersion(1, 1, epoch, boost::none /* timestamp */));
    const auto version3 = ComparableChunkVersion::makeComparableChunkVersion(
        ChunkVersion(2, 0, epoch, boost::none /* timestamp */));
    ASSERT(version1 != version2);
    ASSERT(version1 < version2);
    ASSERT_FALSE(version1 > version2);
    ASSERT(version3 != version2);
    ASSERT(version2 < version3);
    ASSERT_FALSE(version2 > version3);
}

TEST(ComparableChunkVersionTest, DefaultConstructedVersionsAreEqual) {
    const ComparableChunkVersion defaultVersion1{}, defaultVersion2{};
    ASSERT(defaultVersion1 == defaultVersion2);
    ASSERT_FALSE(defaultVersion1 < defaultVersion2);
    ASSERT_FALSE(defaultVersion1 > defaultVersion2);
}

TEST(ComparableChunkVersionTest, DefaultConstructedVersionIsAlwaysLessThanWithChunksVersion) {
    const ComparableChunkVersion defaultVersion{};
    const auto withChunksVersion = ComparableChunkVersion::makeComparableChunkVersion(
        ChunkVersion(1, 0, OID::gen(), boost::none /* timestamp */));
    ASSERT(defaultVersion != withChunksVersion);
    ASSERT(defaultVersion < withChunksVersion);
    ASSERT_FALSE(defaultVersion > withChunksVersion);
}

TEST(ComparableChunkVersionTest, DefaultConstructedVersionIsAlwaysLessThanNoChunksVersion) {
    const ComparableChunkVersion defaultVersion{};
    const auto noChunksVersion = ComparableChunkVersion::makeComparableChunkVersion(
        ChunkVersion(0, 0, OID::gen(), boost::none /* timestamp */));
    ASSERT(defaultVersion != noChunksVersion);
    ASSERT(defaultVersion < noChunksVersion);
    ASSERT_FALSE(defaultVersion > noChunksVersion);
}

TEST(ComparableChunkVersionTest, DefaultConstructedVersionIsAlwaysLessThanUnsharded) {
    const ComparableChunkVersion defaultVersion{};
    const auto version1 =
        ComparableChunkVersion::makeComparableChunkVersion(ChunkVersion::UNSHARDED());
    ASSERT(defaultVersion != version1);
    ASSERT(defaultVersion < version1);
    ASSERT_FALSE(defaultVersion > version1);
}

TEST(ComparableChunkVersionTest, TwoNoChunksVersionsAreTheSame) {
    const auto oid = OID::gen();
    const auto version1 = ComparableChunkVersion::makeComparableChunkVersion(
        ChunkVersion(0, 0, oid, boost::none /* timestamp */));
    const auto version2 = ComparableChunkVersion::makeComparableChunkVersion(
        ChunkVersion(0, 0, oid, boost::none /* timestamp */));
    ASSERT(version1 == version2);
    ASSERT_FALSE(version1 < version2);
    ASSERT_FALSE(version1 > version2);
}

TEST(ComparableChunkVersionTest, NoChunksCompareBySequenceNum) {
    const auto oid = OID::gen();
    const auto version1 = ComparableChunkVersion::makeComparableChunkVersion(
        ChunkVersion(1, 0, oid, boost::none /* timestamp */));

    const auto noChunkSV1 = ComparableChunkVersion::makeComparableChunkVersion(
        ChunkVersion(0, 0, oid, boost::none /* timestamp */));

    ASSERT(version1 != noChunkSV1);
    ASSERT(noChunkSV1 > version1);

    const auto version2 = ComparableChunkVersion::makeComparableChunkVersion(
        ChunkVersion(2, 0, oid, boost::none /* timestamp */));

    ASSERT(version2 != noChunkSV1);
    ASSERT(version2 > noChunkSV1);
}

TEST(ComparableChunkVersionTest, NoChunksGreaterThanUnshardedBySequenceNum) {
    const auto unsharded =
        ComparableChunkVersion::makeComparableChunkVersion(ChunkVersion::UNSHARDED());
    const auto noChunkSV = ComparableChunkVersion::makeComparableChunkVersion(
        ChunkVersion(0, 0, OID::gen(), boost::none /* timestamp */));

    ASSERT(noChunkSV != unsharded);
    ASSERT(noChunkSV > unsharded);
}

TEST(ComparableChunkVersionTest, UnshardedGreaterThanNoChunksBySequenceNum) {
    const auto noChunkSV = ComparableChunkVersion::makeComparableChunkVersion(
        ChunkVersion(0, 0, OID::gen(), boost::none /* timestamp */));
    const auto unsharded =
        ComparableChunkVersion::makeComparableChunkVersion(ChunkVersion::UNSHARDED());

    ASSERT(noChunkSV != unsharded);
    ASSERT(unsharded > noChunkSV);
}

TEST(ComparableChunkVersionTest, NoChunksGreaterThanDefault) {
    const auto noChunkSV = ComparableChunkVersion::makeComparableChunkVersion(
        ChunkVersion(0, 0, OID::gen(), boost::none /* timestamp */));
    const ComparableChunkVersion defaultVersion{};

    ASSERT(noChunkSV != defaultVersion);
    ASSERT(noChunkSV > defaultVersion);
}

TEST(ComparableChunkVersionTest, CompareForcedRefreshVersionVersusValidChunkVersion) {
    auto oid = OID::gen();
    const ComparableChunkVersion defaultVersionBeforeForce;
    const auto versionBeforeForce = ComparableChunkVersion::makeComparableChunkVersion(
        ChunkVersion(100, 0, oid, boost::none /* timestamp */));

    const auto forcedRefreshVersion =
        ComparableChunkVersion::makeComparableChunkVersionForForcedRefresh();

    const auto versionAfterForce = ComparableChunkVersion::makeComparableChunkVersion(
        ChunkVersion(100, 0, oid, boost::none /* timestamp */));
    const ComparableChunkVersion defaultVersionAfterForce;

    ASSERT(defaultVersionBeforeForce != forcedRefreshVersion);
    ASSERT(defaultVersionBeforeForce < forcedRefreshVersion);

    ASSERT(versionBeforeForce != forcedRefreshVersion);
    ASSERT(versionBeforeForce < forcedRefreshVersion);

    ASSERT(versionAfterForce != forcedRefreshVersion);
    ASSERT(versionAfterForce > forcedRefreshVersion);

    ASSERT(defaultVersionAfterForce != forcedRefreshVersion);
    ASSERT(defaultVersionAfterForce < forcedRefreshVersion);
}

TEST(ComparableChunkVersionTest, CompareTwoForcedRefreshVersions) {
    const auto forcedRefreshVersion1 =
        ComparableChunkVersion::makeComparableChunkVersionForForcedRefresh();
    ASSERT(forcedRefreshVersion1 == forcedRefreshVersion1);
    ASSERT_FALSE(forcedRefreshVersion1 < forcedRefreshVersion1);
    ASSERT_FALSE(forcedRefreshVersion1 > forcedRefreshVersion1);

    const auto forcedRefreshVersion2 =
        ComparableChunkVersion::makeComparableChunkVersionForForcedRefresh();
    ASSERT_FALSE(forcedRefreshVersion1 == forcedRefreshVersion2);
    ASSERT(forcedRefreshVersion1 < forcedRefreshVersion2);
    ASSERT_FALSE(forcedRefreshVersion1 > forcedRefreshVersion2);
}

}  // namespace
}  // namespace mongo
