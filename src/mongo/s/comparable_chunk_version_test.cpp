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
    auto versionsEqual = [](const ChunkVersion& v1, const ChunkVersion& v2) {
        const auto version1 = ComparableChunkVersion::makeComparableChunkVersion(v1);
        const auto version2 = ComparableChunkVersion::makeComparableChunkVersion(v2);
        ASSERT(version1 == version2);
    };

    const auto epoch = OID::gen();
    versionsEqual(ChunkVersion(1, 0, epoch, boost::none /* timestamp */),
                  ChunkVersion(1, 0, epoch, boost::none /* timestamp */));
    versionsEqual(ChunkVersion(1, 0, epoch, Timestamp(1)), ChunkVersion(1, 0, epoch, Timestamp(1)));
}

TEST(ComparableChunkVersionTest, VersionsEqualAfterCopy) {
    auto equalAfterCopy = [](const ChunkVersion& v) {
        const auto version1 = ComparableChunkVersion::makeComparableChunkVersion(v);
        const auto version2 = version1;
        ASSERT(version1 == version2);
    };

    equalAfterCopy(ChunkVersion(1, 0, OID::gen(), boost::none /* timestamp */));
    equalAfterCopy(ChunkVersion(1, 0, OID::gen(), Timestamp(1)));
}

TEST(ComparableChunkVersionTest, CompareDifferentVersionsEpochsOrTimestamps) {
    auto compareDifferentVersions = [](const ChunkVersion& v1, const ChunkVersion& v2) {
        const auto version1 = ComparableChunkVersion::makeComparableChunkVersion(v1);
        const auto version2 = ComparableChunkVersion::makeComparableChunkVersion(v2);
        ASSERT(version2 != version1);
        ASSERT(version2 > version1);
        ASSERT_FALSE(version2 < version1);
    };

    compareDifferentVersions(ChunkVersion(2, 0, OID::gen(), boost::none /* timestamp */),
                             ChunkVersion(1, 0, OID::gen(), boost::none /* timestamp */));
    compareDifferentVersions(ChunkVersion(2, 0, OID::gen(), Timestamp(1)),
                             ChunkVersion(1, 0, OID::gen(), Timestamp(2)));
}

TEST(ComparableChunkVersionTest, CompareDifferentVersionsTimestampsIgnoreSequenceNumber) {
    const auto version1 = ComparableChunkVersion::makeComparableChunkVersion(
        ChunkVersion(2, 0, OID::gen(), Timestamp(2)));
    const auto version2 = ComparableChunkVersion::makeComparableChunkVersion(
        ChunkVersion(2, 0, OID::gen(), Timestamp(1)));
    ASSERT(version1 != version2);
    ASSERT(version1 > version2);
    ASSERT_FALSE(version1 < version2);
}

TEST(ComparableChunkVersionTest, VersionGreaterSameEpochsOrTimestamps) {
    auto greaterSameEpochsOrTimestamps =
        [](const ChunkVersion& v1, const ChunkVersion& v2, const ChunkVersion& v3) {
            const auto version1 = ComparableChunkVersion::makeComparableChunkVersion(v1);
            const auto version2 = ComparableChunkVersion::makeComparableChunkVersion(v2);
            const auto version3 = ComparableChunkVersion::makeComparableChunkVersion(v3);
            ASSERT(version2 != version1);
            ASSERT(version2 > version1);
            ASSERT_FALSE(version2 < version1);
            ASSERT(version3 != version2);
            ASSERT(version3 > version2);
            ASSERT_FALSE(version3 < version2);
        };

    const auto epoch = OID::gen();
    greaterSameEpochsOrTimestamps(ChunkVersion(1, 0, epoch, boost::none /* timestamp */),
                                  ChunkVersion(1, 1, epoch, boost::none /* timestamp */),
                                  ChunkVersion(2, 0, epoch, boost::none /* timestamp */));
    const Timestamp timestamp(1);
    greaterSameEpochsOrTimestamps(ChunkVersion(1, 0, epoch, timestamp),
                                  ChunkVersion(1, 1, epoch, timestamp),
                                  ChunkVersion(2, 0, epoch, timestamp));
}

TEST(ComparableChunkVersionTest, VersionLessSameEpochsOrTimestamps) {
    auto lessSameEpochsOrTimestamps =
        [](const ChunkVersion& v1, const ChunkVersion& v2, const ChunkVersion& v3) {
            const auto version1 = ComparableChunkVersion::makeComparableChunkVersion(v1);
            const auto version2 = ComparableChunkVersion::makeComparableChunkVersion(v2);
            const auto version3 = ComparableChunkVersion::makeComparableChunkVersion(v3);
            ASSERT(version1 != version2);
            ASSERT(version1 < version2);
            ASSERT_FALSE(version1 > version2);
            ASSERT(version3 != version2);
            ASSERT(version2 < version3);
            ASSERT_FALSE(version2 > version3);
        };

    const auto epoch = OID::gen();
    lessSameEpochsOrTimestamps(ChunkVersion(1, 0, epoch, boost::none /* timestamp */),
                               ChunkVersion(1, 1, epoch, boost::none /* timestamp */),
                               ChunkVersion(2, 0, epoch, boost::none /* timestamp */));
    const Timestamp timestamp(1);
    lessSameEpochsOrTimestamps(ChunkVersion(1, 0, epoch, timestamp),
                               ChunkVersion(1, 1, epoch, timestamp),
                               ChunkVersion(2, 0, epoch, timestamp));
}

TEST(ComparableChunkVersionTest, compareEpochBasedVersionAgainstEpochAndTimestampBasedVersion) {
    {
        auto equalVersions = [](const ChunkVersion& v1, const ChunkVersion& v2) {
            const auto version1 = ComparableChunkVersion::makeComparableChunkVersion(v1);
            const auto version2 = ComparableChunkVersion::makeComparableChunkVersion(v2);
            ASSERT(version1 == version2);
            ASSERT_FALSE(version1 < version2);
            ASSERT_FALSE(version1 > version2);
        };

        const auto epoch = OID::gen();
        equalVersions(ChunkVersion(1, 0, epoch, boost::none /* timestamp */),
                      ChunkVersion(1, 0, epoch, Timestamp(1)));
        equalVersions(ChunkVersion(1, 0, epoch, Timestamp(1)),
                      ChunkVersion(1, 0, epoch, boost::none /* timestamp */));
    }
    {
        auto diffVersionsMoreRecentByMajor = [](const ChunkVersion& v1, const ChunkVersion& v2) {
            const auto version1 = ComparableChunkVersion::makeComparableChunkVersion(v1);
            const auto version2 = ComparableChunkVersion::makeComparableChunkVersion(v2);
            ASSERT(version1 != version2);
            ASSERT(version1 > version2);
        };

        const auto epoch = OID::gen();
        diffVersionsMoreRecentByMajor(ChunkVersion(2, 0, epoch, boost::none /* timestamp */),
                                      ChunkVersion(1, 0, epoch, Timestamp(1)));
        diffVersionsMoreRecentByMajor(ChunkVersion(2, 0, epoch, Timestamp(1)),
                                      ChunkVersion(1, 0, epoch, boost::none /* timestamp */));
    }
    {
        auto diffVersionsMoreRecentByDisambigSeqNum = [](const ChunkVersion& v1,
                                                         const ChunkVersion& v2) {
            const auto version1 = ComparableChunkVersion::makeComparableChunkVersion(v1);
            const auto version2 = ComparableChunkVersion::makeComparableChunkVersion(v2);
            ASSERT(version1 != version2);
            ASSERT(version1 < version2);
        };

        const auto epoch1 = OID::gen();
        const auto epoch2 = OID::gen();
        diffVersionsMoreRecentByDisambigSeqNum(
            ChunkVersion(2, 0, epoch1, boost::none /* timestamp */),
            ChunkVersion(1, 0, epoch2, Timestamp(1)));
        diffVersionsMoreRecentByDisambigSeqNum(
            ChunkVersion(1, 0, epoch1, Timestamp(1)),
            ChunkVersion(2, 0, epoch2, boost::none /* timestamp */));
    }
}

TEST(ComparableChunkVersionTest, DefaultConstructedVersionsAreEqual) {
    const ComparableChunkVersion defaultVersion1{}, defaultVersion2{};
    ASSERT(defaultVersion1 == defaultVersion2);
    ASSERT_FALSE(defaultVersion1 < defaultVersion2);
    ASSERT_FALSE(defaultVersion1 > defaultVersion2);
}

TEST(ComparableChunkVersionTest, DefaultConstructedVersionIsAlwaysLessThanWithChunksVersion) {
    auto defaultConstructedIsLessThanWithChunks = [](const ChunkVersion& v) {
        const ComparableChunkVersion defaultVersion{};
        const auto withChunksVersion = ComparableChunkVersion::makeComparableChunkVersion(v);
        ASSERT(defaultVersion != withChunksVersion);
        ASSERT(defaultVersion < withChunksVersion);
        ASSERT_FALSE(defaultVersion > withChunksVersion);
    };

    defaultConstructedIsLessThanWithChunks(
        ChunkVersion(1, 0, OID::gen(), boost::none /* timestamp */));
    defaultConstructedIsLessThanWithChunks(ChunkVersion(1, 0, OID::gen(), Timestamp(1)));
}

TEST(ComparableChunkVersionTest, DefaultConstructedVersionIsAlwaysLessThanNoChunksVersion) {
    auto defaultConstructedIsLessThanNoChunks = [](const ChunkVersion& v) {
        const ComparableChunkVersion defaultVersion{};
        const auto noChunksVersion = ComparableChunkVersion::makeComparableChunkVersion(v);
        ASSERT(defaultVersion != noChunksVersion);
        ASSERT(defaultVersion < noChunksVersion);
        ASSERT_FALSE(defaultVersion > noChunksVersion);
    };

    defaultConstructedIsLessThanNoChunks(
        ChunkVersion(0, 0, OID::gen(), boost::none /* timestamp */));
    defaultConstructedIsLessThanNoChunks(ChunkVersion(0, 0, OID::gen(), Timestamp(1)));
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
    auto twoNoChunkVersionsAreTheSame = [](const ChunkVersion& v1, const ChunkVersion& v2) {
        const auto noChunksVersion1 = ComparableChunkVersion::makeComparableChunkVersion(v1);
        const auto noChunksVersion2 = ComparableChunkVersion::makeComparableChunkVersion(v2);
        ASSERT(noChunksVersion1 == noChunksVersion2);
        ASSERT_FALSE(noChunksVersion1 < noChunksVersion2);
        ASSERT_FALSE(noChunksVersion1 > noChunksVersion2);
    };

    const auto oid = OID::gen();
    twoNoChunkVersionsAreTheSame(ChunkVersion(0, 0, oid, boost::none /* timestamp */),
                                 ChunkVersion(0, 0, oid, boost::none /* timestamp */));
    twoNoChunkVersionsAreTheSame(ChunkVersion(0, 0, oid, Timestamp(1)),
                                 ChunkVersion(0, 0, oid, Timestamp(1)));
}

TEST(ComparableChunkVersionTest, NoChunksComparedBySequenceNum) {
    auto noChunksComparedBySequenceNum =
        [](const ChunkVersion& v1, const ChunkVersion& v2, const ChunkVersion& v3) {
            const auto version1 = ComparableChunkVersion::makeComparableChunkVersion(v1);
            const auto noChunksVersion2 = ComparableChunkVersion::makeComparableChunkVersion(v2);
            const auto version3 = ComparableChunkVersion::makeComparableChunkVersion(v3);
            ASSERT(version1 != noChunksVersion2);
            ASSERT(noChunksVersion2 > version1);
            ASSERT(version3 != noChunksVersion2);
            ASSERT(version3 > noChunksVersion2);
        };

    const auto oid = OID::gen();
    noChunksComparedBySequenceNum(ChunkVersion(1, 0, oid, boost::none /* timestamp */),
                                  ChunkVersion(0, 0, oid, boost::none /* timestamp */),
                                  ChunkVersion(2, 0, oid, boost::none /* timestamp */));
    noChunksComparedBySequenceNum(ChunkVersion(1, 0, oid, Timestamp(1)),
                                  ChunkVersion(0, 0, oid, Timestamp(1)),
                                  ChunkVersion(2, 0, oid, Timestamp(1)));
}

TEST(ComparableChunkVersionTest, NoChunksGreaterThanUnshardedBySequenceNum) {
    auto noChunksGreaterThanUnshardedBySequenceNum = [](const ChunkVersion& v) {
        const auto unsharded =
            ComparableChunkVersion::makeComparableChunkVersion(ChunkVersion::UNSHARDED());
        const auto noChunkSV = ComparableChunkVersion::makeComparableChunkVersion(v);
        ASSERT(noChunkSV != unsharded);
        ASSERT(noChunkSV > unsharded);
    };

    noChunksGreaterThanUnshardedBySequenceNum(
        ChunkVersion(0, 0, OID::gen(), boost::none /* timestamp */));
    noChunksGreaterThanUnshardedBySequenceNum(ChunkVersion(0, 0, OID::gen(), Timestamp(1)));
}

TEST(ComparableChunkVersionTest, UnshardedGreaterThanNoChunksBySequenceNum) {
    auto unshardedGreaterThanNoChunksBySequenceNum = [](const ChunkVersion& v) {
        const auto noChunkSV = ComparableChunkVersion::makeComparableChunkVersion(v);
        const auto unsharded =
            ComparableChunkVersion::makeComparableChunkVersion(ChunkVersion::UNSHARDED());
        ASSERT(noChunkSV != unsharded);
        ASSERT(unsharded > noChunkSV);
    };

    unshardedGreaterThanNoChunksBySequenceNum(
        ChunkVersion(0, 0, OID::gen(), boost::none /* timestamp */));
    unshardedGreaterThanNoChunksBySequenceNum(ChunkVersion(0, 0, OID::gen(), Timestamp(1)));
}

TEST(ComparableChunkVersionTest, NoChunksGreaterThanDefault) {
    auto noChunksGreaterThanDefault = [](const ChunkVersion& v) {
        const auto noChunkSV = ComparableChunkVersion::makeComparableChunkVersion(v);
        const ComparableChunkVersion defaultVersion{};
        ASSERT(noChunkSV != defaultVersion);
        ASSERT(noChunkSV > defaultVersion);
    };

    noChunksGreaterThanDefault(ChunkVersion(0, 0, OID::gen(), boost::none /* timestamp */));
    noChunksGreaterThanDefault(ChunkVersion(0, 0, OID::gen(), Timestamp(1)));
}

TEST(ComparableChunkVersionTest, CompareForcedRefreshVersionVersusValidChunkVersion) {
    auto compareForcedRefreshVersionVersusValidChunkVersion = [](const ChunkVersion& v) {
        const ComparableChunkVersion defaultVersionBeforeForce;
        const auto versionBeforeForce = ComparableChunkVersion::makeComparableChunkVersion(v);
        const auto forcedRefreshVersion =
            ComparableChunkVersion::makeComparableChunkVersionForForcedRefresh();
        const auto versionAfterForce = ComparableChunkVersion::makeComparableChunkVersion(v);
        const ComparableChunkVersion defaultVersionAfterForce;

        ASSERT(defaultVersionBeforeForce != forcedRefreshVersion);
        ASSERT(defaultVersionBeforeForce < forcedRefreshVersion);

        ASSERT(versionBeforeForce != forcedRefreshVersion);
        ASSERT(versionBeforeForce < forcedRefreshVersion);

        ASSERT(versionAfterForce != forcedRefreshVersion);
        ASSERT(versionAfterForce > forcedRefreshVersion);

        ASSERT(defaultVersionAfterForce != forcedRefreshVersion);
        ASSERT(defaultVersionAfterForce < forcedRefreshVersion);
    };

    compareForcedRefreshVersionVersusValidChunkVersion(
        ChunkVersion(100, 0, OID::gen(), boost::none /* timestamp */));
    compareForcedRefreshVersionVersusValidChunkVersion(
        ChunkVersion(100, 0, OID::gen(), Timestamp(1)));
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

TEST(ComparableChunkVersionTest, CompareTwoVersionsWithVersionedForcedRefresh) {
    auto compareTwoVersionsWithVersionedForcedRefresh = [](const ChunkVersion& oldV,
                                                           const ChunkVersion& newV) {
        const auto newVersionBeforeForce = ComparableChunkVersion::makeComparableChunkVersion(newV);
        const auto oldVersionBeforeForce = ComparableChunkVersion::makeComparableChunkVersion(oldV);
        ASSERT(oldVersionBeforeForce < newVersionBeforeForce);

        const auto oldVersionWithForce =
            ComparableChunkVersion::makeComparableChunkVersionForForcedRefresh(oldV);
        ASSERT(oldVersionBeforeForce < oldVersionWithForce);
        ASSERT(newVersionBeforeForce < oldVersionWithForce);

        const auto newVersionAfterForce = ComparableChunkVersion::makeComparableChunkVersion(newV);
        const auto oldVersionAfterForce = ComparableChunkVersion::makeComparableChunkVersion(oldV);
        ASSERT(oldVersionAfterForce < newVersionAfterForce);

        ASSERT(newVersionBeforeForce != newVersionAfterForce);
        ASSERT(oldVersionBeforeForce != oldVersionAfterForce);

        ASSERT(oldVersionWithForce < newVersionAfterForce);
        ASSERT_FALSE(oldVersionWithForce < oldVersionAfterForce);
        ASSERT_FALSE(oldVersionWithForce > oldVersionAfterForce);
        ASSERT(oldVersionWithForce == oldVersionAfterForce);
    };

    const auto epoch = OID::gen();
    compareTwoVersionsWithVersionedForcedRefresh(
        ChunkVersion(100, 0, epoch, boost::none /* timestamp */),
        ChunkVersion(100, 1, epoch, boost::none /* timestamp */));

    compareTwoVersionsWithVersionedForcedRefresh(ChunkVersion(100, 0, epoch, Timestamp(1)),
                                                 ChunkVersion(100, 1, epoch, Timestamp(1)));
}

}  // namespace
}  // namespace mongo
