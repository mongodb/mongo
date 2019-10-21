
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <limits>

#include "mongo/db/jsobj.h"
#include "mongo/s/chunk_version.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {


TEST(Parsing, EpochIsOptional) {
    const OID oid = OID::gen();
    bool canParse = false;

    ChunkVersion chunkVersionComplete = ChunkVersion::fromBSON(
        BSON("lastmod" << Timestamp(Seconds(2), 3) << "lastmodEpoch" << oid), "lastmod", &canParse);
    ASSERT(canParse);
    ASSERT(chunkVersionComplete.epoch().isSet());
    ASSERT(chunkVersionComplete.epoch() == oid);
    ASSERT_EQ(2u, chunkVersionComplete.majorVersion());
    ASSERT_EQ(3u, chunkVersionComplete.minorVersion());

    canParse = false;
    ChunkVersion chunkVersionNoEpoch =
        ChunkVersion::fromBSON(BSON("lastmod" << Timestamp(Seconds(3), 4)), "lastmod", &canParse);
    ASSERT(canParse);
    ASSERT(!chunkVersionNoEpoch.epoch().isSet());
    ASSERT_EQ(3u, chunkVersionNoEpoch.majorVersion());
    ASSERT_EQ(4u, chunkVersionNoEpoch.minorVersion());
}

TEST(Comparison, StrictEqual) {
    OID epoch = OID::gen();

    ASSERT(ChunkVersion(3, 1, epoch).isStrictlyEqualTo(ChunkVersion(3, 1, epoch)));
    ASSERT(!ChunkVersion(3, 1, epoch).isStrictlyEqualTo(ChunkVersion(3, 1, OID())));
    ASSERT(!ChunkVersion(3, 1, OID()).isStrictlyEqualTo(ChunkVersion(3, 1, epoch)));
    ASSERT(ChunkVersion(3, 1, OID()).isStrictlyEqualTo(ChunkVersion(3, 1, OID())));
    ASSERT(!ChunkVersion(4, 2, epoch).isStrictlyEqualTo(ChunkVersion(4, 1, epoch)));
}

TEST(Comparison, OlderThan) {
    OID epoch = OID::gen();

    ASSERT(ChunkVersion(3, 1, epoch).isOlderThan(ChunkVersion(4, 1, epoch)));
    ASSERT(!ChunkVersion(4, 1, epoch).isOlderThan(ChunkVersion(3, 1, epoch)));

    ASSERT(ChunkVersion(3, 1, epoch).isOlderThan(ChunkVersion(3, 2, epoch)));
    ASSERT(!ChunkVersion(3, 2, epoch).isOlderThan(ChunkVersion(3, 1, epoch)));

    ASSERT(!ChunkVersion(3, 1, epoch).isOlderThan(ChunkVersion(4, 1, OID())));
    ASSERT(!ChunkVersion(4, 1, OID()).isOlderThan(ChunkVersion(3, 1, epoch)));

    ASSERT(ChunkVersion(3, 2, epoch).isOlderThan(ChunkVersion(4, 1, epoch)));

    ASSERT(!ChunkVersion(3, 1, epoch).isOlderThan(ChunkVersion(3, 1, epoch)));
}

TEST(ChunkVersionConstruction, CreateWithLargeValues) {
    const auto minorVersion = std::numeric_limits<uint32_t>::max();
    const uint32_t majorVersion = 1 << 24;
    const auto epoch = OID::gen();

    ChunkVersion version(majorVersion, minorVersion, epoch);
    ASSERT_EQ(majorVersion, version.majorVersion());
    ASSERT_EQ(minorVersion, version.minorVersion());
    ASSERT_EQ(epoch, version.epoch());
}

TEST(ChunkVersionManipulation, ThrowsErrorIfOverflowIsAttemptedForMajorVersion) {
    const uint32_t minorVersion = 0;
    const uint32_t majorVersion = std::numeric_limits<uint32_t>::max();
    const auto epoch = OID::gen();

    ChunkVersion version(majorVersion, minorVersion, epoch);
    ASSERT_EQ(majorVersion, version.majorVersion());
    ASSERT_EQ(minorVersion, version.minorVersion());
    ASSERT_EQ(epoch, version.epoch());

    ASSERT_THROWS_CODE(version.incMajor(), DBException, 31180);
}

TEST(ChunkVersionManipulation, ThrowsErrorIfOverflowIsAttemptedForMinorVersion) {
    const uint32_t minorVersion = std::numeric_limits<uint32_t>::max();
    const uint32_t majorVersion = 0;
    const auto epoch = OID::gen();

    ChunkVersion version(majorVersion, minorVersion, epoch);
    ASSERT_EQ(majorVersion, version.majorVersion());
    ASSERT_EQ(minorVersion, version.minorVersion());
    ASSERT_EQ(epoch, version.epoch());

    ASSERT_THROWS_CODE(version.incMinor(), DBException, 31181);
}
}  // namespace
}  // namespace mongo
