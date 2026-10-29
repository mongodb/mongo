// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/unittest/unittest.h"

#include <memory>

namespace mongo {
namespace {

TEST(ComparableChunkVersionTest, VersionsEqual) {
    const auto epoch = OID::gen();
    const Timestamp timestamp(1, 1);
    const ChunkVersion v1({epoch, timestamp}, {1, 0});
    const ChunkVersion v2({epoch, timestamp}, {1, 0});
    const auto version1 = ComparableChunkVersion::makeComparableChunkVersion(v1);
    const auto version2 = ComparableChunkVersion::makeComparableChunkVersion(v2);
    ASSERT(version1 == version2);
}

TEST(ComparableChunkVersionTest, VersionsEqualAfterCopy) {
    const ChunkVersion chunkVersion({OID::gen(), Timestamp(1, 1)}, {1, 0});
    const auto version1 = ComparableChunkVersion::makeComparableChunkVersion(chunkVersion);
    const auto version2 = version1;
    ASSERT(version1 == version2);
}


TEST(ComparableChunkVersionTest, CompareDifferentTimestamps) {
    const ChunkVersion v1({OID::gen(), Timestamp(1)}, {2, 0});
    const ChunkVersion v2({OID::gen(), Timestamp(2)}, {1, 0});
    const auto version1 = ComparableChunkVersion::makeComparableChunkVersion(v1);
    const auto version2 = ComparableChunkVersion::makeComparableChunkVersion(v2);
    ASSERT(version2 != version1);
    ASSERT(version2 > version1);
    ASSERT_FALSE(version2 < version1);
}

TEST(ComparableChunkVersionTest, CompareDifferentVersionsTimestampsIgnoreSequenceNumber) {
    const auto version1 = ComparableChunkVersion::makeComparableChunkVersion(
        ChunkVersion({OID::gen(), Timestamp(2)}, {2, 0}));
    const auto version2 = ComparableChunkVersion::makeComparableChunkVersion(
        ChunkVersion({OID::gen(), Timestamp(1)}, {2, 0}));
    ASSERT(version1 != version2);
    ASSERT(version1 > version2);
    ASSERT_FALSE(version1 < version2);
}

TEST(ComparableChunkVersionTest, VersionGreaterSameTimestamps) {
    const auto epoch = OID::gen();
    const Timestamp timestamp(1, 1);
    const ChunkVersion v1({epoch, timestamp}, {1, 0});
    const ChunkVersion v2({epoch, timestamp}, {1, 2});
    const ChunkVersion v3({epoch, timestamp}, {2, 0});
    const auto version1 = ComparableChunkVersion::makeComparableChunkVersion(v1);
    const auto version2 = ComparableChunkVersion::makeComparableChunkVersion(v2);
    const auto version3 = ComparableChunkVersion::makeComparableChunkVersion(v3);
    ASSERT(version2 != version1);
    ASSERT(version2 > version1);
    ASSERT_FALSE(version2 < version1);
    ASSERT(version3 != version2);
    ASSERT(version3 > version2);
    ASSERT_FALSE(version3 < version2);
}

TEST(ComparableChunkVersionTest, VersionLessSameTimestamps) {
    const auto epoch = OID::gen();
    const Timestamp timestamp(1, 1);
    const ChunkVersion v1({epoch, timestamp}, {1, 0});
    const ChunkVersion v2({epoch, timestamp}, {1, 2});
    const ChunkVersion v3({epoch, timestamp}, {2, 0});
    const auto version1 = ComparableChunkVersion::makeComparableChunkVersion(v1);
    const auto version2 = ComparableChunkVersion::makeComparableChunkVersion(v2);
    const auto version3 = ComparableChunkVersion::makeComparableChunkVersion(v3);
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
    const ChunkVersion chunkVersion({OID::gen(), Timestamp(1, 1)}, {1, 0});
    const ComparableChunkVersion defaultVersion{};
    const auto withChunksVersion = ComparableChunkVersion::makeComparableChunkVersion(chunkVersion);
    ASSERT(defaultVersion != withChunksVersion);
    ASSERT(defaultVersion < withChunksVersion);
    ASSERT_FALSE(defaultVersion > withChunksVersion);
}

TEST(ComparableChunkVersionTest, DefaultConstructedVersionIsAlwaysLessThanNoChunksVersion) {
    const ChunkVersion chunkVersion({OID::gen(), Timestamp(1, 1)}, {0, 0});
    const ComparableChunkVersion defaultVersion{};
    const auto noChunksVersion = ComparableChunkVersion::makeComparableChunkVersion(chunkVersion);
    ASSERT(defaultVersion != noChunksVersion);
    ASSERT(defaultVersion < noChunksVersion);
    ASSERT_FALSE(defaultVersion > noChunksVersion);
}

TEST(ComparableChunkVersionTest, DefaultConstructedVersionIsAlwaysLessThanUnsharded) {
    const ComparableChunkVersion defaultVersion{};
    const auto version1 =
        ComparableChunkVersion::makeComparableChunkVersion(ChunkVersion::UNTRACKED());
    ASSERT(defaultVersion != version1);
    ASSERT(defaultVersion < version1);
    ASSERT_FALSE(defaultVersion > version1);
}

TEST(ComparableChunkVersionTest, TwoNoChunksVersionsAreTheSame) {
    const auto oid = OID::gen();
    const ChunkVersion v1({oid, Timestamp(1, 1)}, {0, 0});
    const ChunkVersion v2({oid, Timestamp(1, 1)}, {0, 0});
    const auto noChunksVersion1 = ComparableChunkVersion::makeComparableChunkVersion(v1);
    const auto noChunksVersion2 = ComparableChunkVersion::makeComparableChunkVersion(v2);
    ASSERT(noChunksVersion1 == noChunksVersion2);
    ASSERT_FALSE(noChunksVersion1 < noChunksVersion2);
    ASSERT_FALSE(noChunksVersion1 > noChunksVersion2);
}

TEST(ComparableChunkVersionTest, NoChunksComparedBySequenceNum) {
    const auto oid = OID::gen();
    const Timestamp timestamp(1);
    const ChunkVersion v1({oid, timestamp}, {1, 0});
    const ChunkVersion v2({oid, timestamp}, {0, 0});
    const ChunkVersion v3({oid, timestamp}, {2, 0});
    const auto version1 = ComparableChunkVersion::makeComparableChunkVersion(v1);
    const auto noChunksVersion2 = ComparableChunkVersion::makeComparableChunkVersion(v2);
    const auto version3 = ComparableChunkVersion::makeComparableChunkVersion(v3);
    ASSERT(version1 != noChunksVersion2);
    ASSERT(noChunksVersion2 > version1);
    ASSERT(version3 != noChunksVersion2);
    ASSERT(version3 > noChunksVersion2);
}

TEST(ComparableChunkVersionTest, NoChunksGreaterThanUnshardedBySequenceNum) {
    const ChunkVersion chunkVersion({OID::gen(), Timestamp(1)}, {0, 0});
    const auto unsharded =
        ComparableChunkVersion::makeComparableChunkVersion(ChunkVersion::UNTRACKED());
    const auto noChunkSV = ComparableChunkVersion::makeComparableChunkVersion(chunkVersion);
    ASSERT(noChunkSV != unsharded);
    ASSERT(noChunkSV > unsharded);
}

TEST(ComparableChunkVersionTest, UnshardedGreaterThanNoChunksBySequenceNum) {
    const ChunkVersion chunkVersion({OID::gen(), Timestamp(1)}, {0, 0});
    const auto noChunkSV = ComparableChunkVersion::makeComparableChunkVersion(chunkVersion);
    const auto unsharded =
        ComparableChunkVersion::makeComparableChunkVersion(ChunkVersion::UNTRACKED());
    ASSERT(noChunkSV != unsharded);
    ASSERT(unsharded > noChunkSV);
}

TEST(ComparableChunkVersionTest, NoChunksGreaterThanDefault) {
    const ChunkVersion chunkVersion({OID::gen(), Timestamp(1)}, {0, 0});
    const auto noChunkSV = ComparableChunkVersion::makeComparableChunkVersion(chunkVersion);
    const ComparableChunkVersion defaultVersion{};
    ASSERT(noChunkSV != defaultVersion);
    ASSERT(noChunkSV > defaultVersion);
}

TEST(ComparableChunkVersionTest, CompareForcedRefreshVersionVersusValidChunkVersion) {
    const ChunkVersion chunkVersion({OID::gen(), Timestamp(1)}, {100, 0});
    const ComparableChunkVersion defaultVersionBeforeForce;
    const auto versionBeforeForce =
        ComparableChunkVersion::makeComparableChunkVersion(chunkVersion);
    const auto forcedRefreshVersion =
        ComparableChunkVersion::makeComparableChunkVersionForForcedRefresh();
    const auto versionAfterForce = ComparableChunkVersion::makeComparableChunkVersion(chunkVersion);
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
