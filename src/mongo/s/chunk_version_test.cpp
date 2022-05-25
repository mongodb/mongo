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

#include <limits>

#include "mongo/s/chunk_version.h"
#include "mongo/s/chunk_version_gen.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(ChunkVersionTest, EqualityOperators) {
    OID epoch = OID::gen();
    Timestamp timestamp = Timestamp(1);

    ASSERT_EQ(ChunkVersion(3, 1, epoch, Timestamp(1, 1)),
              ChunkVersion(3, 1, epoch, Timestamp(1, 1)));
    ASSERT_EQ(ChunkVersion(3, 1, OID(), timestamp), ChunkVersion(3, 1, OID(), timestamp));

    ASSERT_NE(ChunkVersion(3, 1, epoch, timestamp), ChunkVersion(3, 1, OID(), Timestamp(1, 1)));
    ASSERT_NE(ChunkVersion(3, 1, OID(), Timestamp(1, 1)), ChunkVersion(3, 1, epoch, timestamp));
    ASSERT_NE(ChunkVersion(4, 2, epoch, timestamp), ChunkVersion(4, 1, epoch, timestamp));
}

TEST(ChunkVersionTest, OlderThan) {
    OID epoch = OID::gen();
    Timestamp timestamp(1);
    Timestamp newerTimestamp(2);

    ASSERT(ChunkVersion(3, 1, epoch, timestamp).isOlderThan(ChunkVersion(4, 1, epoch, timestamp)));
    ASSERT(!ChunkVersion(4, 1, epoch, timestamp).isOlderThan(ChunkVersion(3, 1, epoch, timestamp)));

    ASSERT(ChunkVersion(3, 1, epoch, timestamp).isOlderThan(ChunkVersion(3, 2, epoch, timestamp)));
    ASSERT(!ChunkVersion(3, 2, epoch, timestamp).isOlderThan(ChunkVersion(3, 1, epoch, timestamp)));

    ASSERT(ChunkVersion(3, 1, epoch, timestamp)
               .isOlderThan(ChunkVersion(3, 1, OID::gen(), newerTimestamp)));
    ASSERT(!ChunkVersion(3, 1, epoch, newerTimestamp)
                .isOlderThan(ChunkVersion(3, 1, OID::gen(), timestamp)));

    ASSERT(!ChunkVersion::UNSHARDED().isOlderThan(ChunkVersion(3, 1, epoch, timestamp)));
    ASSERT(!ChunkVersion(3, 1, epoch, timestamp).isOlderThan(ChunkVersion::UNSHARDED()));
}

TEST(ChunkVersionTest, CreateWithLargeValues) {
    const uint32_t majorVersion = std::numeric_limits<uint32_t>::max();
    const uint32_t minorVersion = std::numeric_limits<uint32_t>::max();
    const auto epoch = OID::gen();

    ChunkVersion version(majorVersion, minorVersion, epoch, Timestamp(1, 1));
    ASSERT_EQ(majorVersion, version.majorVersion());
    ASSERT_EQ(minorVersion, version.minorVersion());
    ASSERT_EQ(epoch, version.epoch());
    ASSERT_EQ(Timestamp(1, 1), version.getTimestamp());
}

TEST(ChunkVersionTest, ThrowsErrorIfOverflowIsAttemptedForMajorVersion) {
    const uint32_t majorVersion = std::numeric_limits<uint32_t>::max();
    const uint32_t minorVersion = 0;
    const auto epoch = OID::gen();

    ChunkVersion version(majorVersion, minorVersion, epoch, Timestamp(1, 1));
    ASSERT_EQ(majorVersion, version.majorVersion());
    ASSERT_EQ(minorVersion, version.minorVersion());
    ASSERT_EQ(epoch, version.epoch());

    ASSERT_THROWS_CODE(version.incMajor(), DBException, 31180);
}

TEST(ChunkVersionTest, ThrowsErrorIfOverflowIsAttemptedForMinorVersion) {
    const uint32_t majorVersion = 0;
    const uint32_t minorVersion = std::numeric_limits<uint32_t>::max();
    const auto epoch = OID::gen();

    ChunkVersion version(majorVersion, minorVersion, epoch, Timestamp(1, 1));
    ASSERT_EQ(majorVersion, version.majorVersion());
    ASSERT_EQ(minorVersion, version.minorVersion());
    ASSERT_EQ(epoch, version.epoch());

    ASSERT_THROWS_CODE(version.incMinor(), DBException, 31181);
}

}  // namespace
}  // namespace mongo
