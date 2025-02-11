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
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(ChunkVersionTest, EqualityOperators) {
    OID epoch = OID::gen();
    Timestamp timestamp = Timestamp(1);

    ASSERT_EQ(ChunkVersion({epoch, Timestamp(1, 1)}, {3, 1}),
              ChunkVersion({epoch, Timestamp(1, 1)}, {3, 1}));
    ASSERT_EQ(ChunkVersion({OID(), timestamp}, {3, 1}), ChunkVersion({OID(), timestamp}, {3, 1}));

    ASSERT_NE(ChunkVersion({epoch, timestamp}, {3, 1}),
              ChunkVersion({OID(), Timestamp(1, 1)}, {3, 1}));
    ASSERT_NE(ChunkVersion({OID(), Timestamp(1, 1)}, {3, 1}),
              ChunkVersion({epoch, timestamp}, {3, 1}));
    ASSERT_NE(ChunkVersion({epoch, timestamp}, {4, 2}), ChunkVersion({epoch, timestamp}, {4, 1}));
}

TEST(ChunkVersionTest, OlderThan) {
    OID epoch = OID::gen();
    Timestamp timestamp(1);
    Timestamp newerTimestamp(2);

    ASSERT(ChunkVersion({epoch, timestamp}, {3, 1})
               .isOlderThan(ChunkVersion({epoch, timestamp}, {4, 1})));
    ASSERT(!ChunkVersion({epoch, timestamp}, {4, 1})
                .isOlderThan(ChunkVersion({epoch, timestamp}, {3, 1})));

    ASSERT(ChunkVersion({epoch, timestamp}, {3, 1})
               .isOlderThan(ChunkVersion({epoch, timestamp}, {3, 2})));
    ASSERT(!ChunkVersion({epoch, timestamp}, {3, 2})
                .isOlderThan(ChunkVersion({epoch, timestamp}, {3, 1})));

    ASSERT(ChunkVersion({epoch, timestamp}, {3, 1})
               .isOlderThan(ChunkVersion({OID::gen(), newerTimestamp}, {3, 1})));
    ASSERT(!ChunkVersion({epoch, newerTimestamp}, {3, 1})
                .isOlderThan(ChunkVersion({OID::gen(), timestamp}, {3, 1})));

    ASSERT(!ChunkVersion::UNSHARDED().isOlderThan(ChunkVersion({epoch, timestamp}, {3, 1})));
    ASSERT(!ChunkVersion({epoch, timestamp}, {3, 1}).isOlderThan(ChunkVersion::UNSHARDED()));
}

TEST(ChunkVersionTest, CreateWithLargeValues) {
    const uint32_t majorVersion = std::numeric_limits<uint32_t>::max();
    const uint32_t minorVersion = std::numeric_limits<uint32_t>::max();
    const auto epoch = OID::gen();

    ChunkVersion version({epoch, Timestamp(1, 1)}, {majorVersion, minorVersion});
    ASSERT_EQ(majorVersion, version.majorVersion());
    ASSERT_EQ(minorVersion, version.minorVersion());
    ASSERT_EQ(epoch, version.epoch());
    ASSERT_EQ(Timestamp(1, 1), version.getTimestamp());
}

TEST(ChunkVersionTest, ThrowsErrorIfOverflowIsAttemptedForMajorVersion) {
    const uint32_t majorVersion = std::numeric_limits<uint32_t>::max();
    const uint32_t minorVersion = 0;
    const auto epoch = OID::gen();

    ChunkVersion version({epoch, Timestamp(1, 1)}, {majorVersion, minorVersion});
    ASSERT_EQ(majorVersion, version.majorVersion());
    ASSERT_EQ(minorVersion, version.minorVersion());
    ASSERT_EQ(epoch, version.epoch());

    ASSERT_THROWS_CODE(version.incMajor(), DBException, 31180);
}

TEST(ChunkVersionTest, ThrowsErrorIfOverflowIsAttemptedForMinorVersion) {
    const uint32_t majorVersion = 0;
    const uint32_t minorVersion = std::numeric_limits<uint32_t>::max();
    const auto epoch = OID::gen();

    ChunkVersion version({epoch, Timestamp(1, 1)}, {majorVersion, minorVersion});
    ASSERT_EQ(majorVersion, version.majorVersion());
    ASSERT_EQ(minorVersion, version.minorVersion());
    ASSERT_EQ(epoch, version.epoch());

    ASSERT_THROWS_CODE(version.incMinor(), DBException, 31181);
}

}  // namespace
}  // namespace mongo
