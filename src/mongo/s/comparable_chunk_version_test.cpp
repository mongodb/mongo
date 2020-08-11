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
#include "mongo/s/chunk_version.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(ComparableChunkVersionTest, VersionsEqual) {
    auto epoch = OID::gen();
    const auto version1 =
        ComparableChunkVersion::makeComparableChunkVersion(ChunkVersion(1, 0, epoch));
    const auto version2 =
        ComparableChunkVersion::makeComparableChunkVersion(ChunkVersion(1, 0, epoch));
    ASSERT(version1.getVersion() == version2.getVersion());
    ASSERT(version1 == version2);
}

TEST(ComparableChunkVersionTest, VersionsEqualAfterCopy) {
    ChunkVersion chunkVersion(1, 0, OID::gen());
    const auto version1 = ComparableChunkVersion::makeComparableChunkVersion(chunkVersion);
    const auto version2 = version1;
    ASSERT(version1 == version2);
}

TEST(ComparableChunkVersionTest, CompareVersionDifferentEpochs) {
    const auto version1 =
        ComparableChunkVersion::makeComparableChunkVersion(ChunkVersion(2, 0, OID::gen()));
    const auto version2 =
        ComparableChunkVersion::makeComparableChunkVersion(ChunkVersion(1, 0, OID::gen()));
    ASSERT(version2 != version1);
    ASSERT(version2 > version1);
    ASSERT_FALSE(version2 < version1);
}

TEST(ComparableChunkVersionTest, VersionGreaterSameEpochs) {
    const auto epoch = OID::gen();
    const auto version1 =
        ComparableChunkVersion::makeComparableChunkVersion(ChunkVersion(1, 0, epoch));
    const auto version2 =
        ComparableChunkVersion::makeComparableChunkVersion(ChunkVersion(1, 1, epoch));
    const auto version3 =
        ComparableChunkVersion::makeComparableChunkVersion(ChunkVersion(2, 0, epoch));
    ASSERT(version2 != version1);
    ASSERT(version2 > version1);
    ASSERT_FALSE(version2 < version1);
    ASSERT(version3 != version2);
    ASSERT(version3 > version2);
    ASSERT_FALSE(version3 < version2);
}

TEST(ComparableChunkVersionTest, VersionLessSameEpoch) {
    const auto epoch = OID::gen();
    const auto version1 =
        ComparableChunkVersion::makeComparableChunkVersion(ChunkVersion(1, 0, epoch));
    const auto version2 =
        ComparableChunkVersion::makeComparableChunkVersion(ChunkVersion(1, 1, epoch));
    const auto version3 =
        ComparableChunkVersion::makeComparableChunkVersion(ChunkVersion(2, 0, epoch));
    ASSERT(version1 != version2);
    ASSERT(version1 < version2);
    ASSERT_FALSE(version1 > version2);
    ASSERT(version3 != version2);
    ASSERT(version2 < version3);
    ASSERT_FALSE(version2 > version3);
}

TEST(ComparableChunkVersionTest, DefaultConstructedVersionIsAlwaysLess) {
    const ComparableChunkVersion defaultVersion{};
    ASSERT_EQ(defaultVersion.getLocalSequenceNum(), 0);
    const auto version1 =
        ComparableChunkVersion::makeComparableChunkVersion(ChunkVersion(0, 0, OID::gen()));
    ASSERT(defaultVersion != version1);
    ASSERT(defaultVersion < version1);
    ASSERT_FALSE(defaultVersion > version1);
}

}  // namespace
}  // namespace mongo
