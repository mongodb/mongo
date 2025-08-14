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

#include "mongo/db/versioning_protocol/chunk_version.h"

#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <limits>

namespace mongo {
namespace {

TEST(ChunkVersionTest, EqualityOperators) {
    OID epoch = OID::gen();
    Timestamp timestamp = Timestamp(1);
    Timestamp timestamp2 = Timestamp(2);

    ASSERT_EQ(ChunkVersion({epoch, Timestamp(1, 1)}, {3, 1}),
              ChunkVersion({epoch, Timestamp(1, 1)}, {3, 1}));
    ASSERT_EQ(ChunkVersion({OID(), timestamp}, {3, 1}), ChunkVersion({OID(), timestamp}, {3, 1}));

    ASSERT_NE(ChunkVersion({epoch, timestamp}, {3, 1}),
              ChunkVersion({OID(), Timestamp(1, 1)}, {3, 1}));
    ASSERT_NE(ChunkVersion({OID(), Timestamp(1, 1)}, {3, 1}),
              ChunkVersion({epoch, timestamp}, {3, 1}));
    ASSERT_NE(ChunkVersion({epoch, timestamp}, {4, 2}), ChunkVersion({epoch, timestamp}, {4, 1}));
    ASSERT_NE(ChunkVersion({epoch, timestamp}, {3, 1}), ChunkVersion({epoch, timestamp}, {4, 1}));
    ASSERT_NE(ChunkVersion({epoch, timestamp}, {3, 1}), ChunkVersion({epoch, timestamp2}, {3, 1}));

    // Unset versions
    ASSERT_EQ(ChunkVersion({OID(), Timestamp()}, {0, 0}),
              ChunkVersion({OID(), Timestamp()}, {0, 0}));
    ASSERT_NE(ChunkVersion({epoch, timestamp}, {1, 0}), ChunkVersion({OID(), Timestamp()}, {0, 0}));

    // Special versions
    ASSERT_EQ(ChunkVersion::UNSHARDED(), ChunkVersion::UNSHARDED());
    ASSERT_EQ(ChunkVersion::IGNORED(), ChunkVersion::IGNORED());
    ASSERT_NE(ChunkVersion::UNSHARDED(), ChunkVersion::IGNORED());

    // UNSHARDED vs normal versions
    ASSERT_NE(ChunkVersion::UNSHARDED(), ChunkVersion({epoch, timestamp}, {1, 0}));
    ASSERT_NE(ChunkVersion({epoch, timestamp}, {1, 0}), ChunkVersion::UNSHARDED());

    // IGNORED vs normal versions
    ASSERT_NE(ChunkVersion::IGNORED(), ChunkVersion({epoch, timestamp}, {1, 0}));
    ASSERT_NE(ChunkVersion({epoch, timestamp}, {1, 0}), ChunkVersion::IGNORED());
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

DEATH_TEST_REGEX(ChunkVersionTest, Error_00SameCollectionTimestamp, "Tripwire assertion.*664720") {
    OID epoch = OID::gen();
    OID differentEpoch = OID::gen();
    Timestamp timestamp(1);
    ChunkVersion unsetVersionDifferentEpoch({differentEpoch, timestamp}, {0, 0});

    // The error is thrown if collections have matching timestamps, but different epochs.
    unsetVersionDifferentEpoch <=> ChunkVersion({epoch, timestamp}, {3, 1});
}

DEATH_TEST_REGEX(ChunkVersionTest, Error_00SameCollectionEpoch, "Tripwire assertion.*664721") {
    OID epoch = OID::gen();
    Timestamp timestamp(1);
    Timestamp differentTimestamp(2);
    ChunkVersion unsetVersionDifferentEpoch({epoch, differentTimestamp}, {0, 0});

    // The error is thrown if collections different timestamps, but matching epochs.
    unsetVersionDifferentEpoch <=> ChunkVersion({epoch, timestamp}, {3, 1});
}

TEST(ChunkVersionTest, ThreeWayComparisonOperator) {
    OID epoch = OID::gen();
    OID differentEpoch = OID::gen();
    Timestamp timestamp(1);
    Timestamp newerTimestamp(2);

    // Test unordered cases - UNSHARDED versions
    ASSERT_EQ(std::partial_ordering::unordered,
              ChunkVersion::UNSHARDED() <=> ChunkVersion({epoch, timestamp}, {3, 1}));
    ASSERT_EQ(std::partial_ordering::unordered,
              ChunkVersion({epoch, timestamp}, {3, 1}) <=> ChunkVersion::UNSHARDED());
    ASSERT_EQ(std::partial_ordering::unordered,
              ChunkVersion::UNSHARDED() <=> ChunkVersion::UNSHARDED());

    // Test unordered cases - IGNORED versions
    ASSERT_EQ(std::partial_ordering::unordered,
              ChunkVersion::IGNORED() <=> ChunkVersion({epoch, timestamp}, {3, 1}));
    ASSERT_EQ(std::partial_ordering::unordered,
              ChunkVersion({epoch, timestamp}, {3, 1}) <=> ChunkVersion::IGNORED());
    ASSERT_EQ(std::partial_ordering::unordered,
              ChunkVersion::IGNORED() <=> ChunkVersion::IGNORED());

    // Test unordered cases - UNSHARDED vs IGNORED
    ASSERT_EQ(std::partial_ordering::unordered,
              ChunkVersion::UNSHARDED() <=> ChunkVersion::IGNORED());
    ASSERT_EQ(std::partial_ordering::unordered,
              ChunkVersion::IGNORED() <=> ChunkVersion::UNSHARDED());

    // Test unordered cases - unset placement versions from same collection generation
    ASSERT_EQ(std::partial_ordering::unordered,
              ChunkVersion({epoch, timestamp}, {0, 0}) <=>
                  ChunkVersion({epoch, timestamp}, {3, 1}));
    ASSERT_EQ(std::partial_ordering::unordered,
              ChunkVersion({epoch, timestamp}, {3, 1}) <=>
                  ChunkVersion({epoch, timestamp}, {0, 0}));
    ASSERT_EQ(std::partial_ordering::unordered,
              ChunkVersion({epoch, timestamp}, {0, 0}) <=>
                  ChunkVersion({epoch, timestamp}, {0, 0}));

    // Test equivalent cases
    ASSERT_EQ(std::partial_ordering::equivalent,
              ChunkVersion({epoch, timestamp}, {3, 1}) <=>
                  ChunkVersion({epoch, timestamp}, {3, 1}));
    ASSERT_EQ(std::partial_ordering::equivalent,
              ChunkVersion({epoch, timestamp}, {5, 2}) <=>
                  ChunkVersion({epoch, timestamp}, {5, 2}));

    // Test timestamp comparison - less
    ASSERT_EQ(std::partial_ordering::less,
              ChunkVersion({epoch, timestamp}, {3, 1}) <=>
                  ChunkVersion({differentEpoch, newerTimestamp}, {3, 1}));
    ASSERT_EQ(std::partial_ordering::less,
              ChunkVersion({epoch, timestamp}, {4, 2}) <=>
                  ChunkVersion({differentEpoch, newerTimestamp}, {2, 1}));

    // Test timestamp comparison - greater
    ASSERT_EQ(std::partial_ordering::greater,
              ChunkVersion({epoch, newerTimestamp}, {3, 1}) <=>
                  ChunkVersion({differentEpoch, timestamp}, {3, 1}));
    ASSERT_EQ(std::partial_ordering::greater,
              ChunkVersion({epoch, newerTimestamp}, {2, 1}) <=>
                  ChunkVersion({differentEpoch, timestamp}, {4, 2}));

    // Test major version comparison - less (same timestamp)
    ASSERT_EQ(std::partial_ordering::less,
              ChunkVersion({epoch, timestamp}, {3, 1}) <=>
                  ChunkVersion({epoch, timestamp}, {4, 1}));
    ASSERT_EQ(std::partial_ordering::less,
              ChunkVersion({epoch, timestamp}, {1, 5}) <=>
                  ChunkVersion({epoch, timestamp}, {3, 2}));

    // Test major version comparison - greater (same timestamp)
    ASSERT_EQ(std::partial_ordering::greater,
              ChunkVersion({epoch, timestamp}, {4, 1}) <=>
                  ChunkVersion({epoch, timestamp}, {3, 1}));
    ASSERT_EQ(std::partial_ordering::greater,
              ChunkVersion({epoch, timestamp}, {5, 1}) <=>
                  ChunkVersion({epoch, timestamp}, {2, 8}));

    // Test minor version comparison - less (same timestamp and major version)
    ASSERT_EQ(std::partial_ordering::less,
              ChunkVersion({epoch, timestamp}, {3, 1}) <=>
                  ChunkVersion({epoch, timestamp}, {3, 2}));
    ASSERT_EQ(std::partial_ordering::less,
              ChunkVersion({epoch, timestamp}, {5, 3}) <=>
                  ChunkVersion({epoch, timestamp}, {5, 7}));

    // Test minor version comparison - greater (same timestamp and major version)
    ASSERT_EQ(std::partial_ordering::greater,
              ChunkVersion({epoch, timestamp}, {3, 2}) <=>
                  ChunkVersion({epoch, timestamp}, {3, 1}));
    ASSERT_EQ(std::partial_ordering::greater,
              ChunkVersion({epoch, timestamp}, {5, 7}) <=>
                  ChunkVersion({epoch, timestamp}, {5, 3}));

    // Test different collection generations
    ASSERT_EQ(std::partial_ordering::less,
              ChunkVersion({epoch, timestamp}, {3, 1}) <=>
                  ChunkVersion({differentEpoch, timestamp}, {4, 1}));
    ASSERT_EQ(std::partial_ordering::greater,
              ChunkVersion({epoch, timestamp}, {4, 1}) <=>
                  ChunkVersion({differentEpoch, timestamp}, {3, 1}));

    // Test unset versions from different collection generations (should be comparable)
    ASSERT_EQ(std::partial_ordering::less,
              ChunkVersion({epoch, timestamp}, {0, 0}) <=>
                  ChunkVersion({differentEpoch, newerTimestamp}, {3, 1}));
    ASSERT_EQ(std::partial_ordering::less,
              ChunkVersion({epoch, timestamp}, {4, 2}) <=>
                  ChunkVersion({differentEpoch, newerTimestamp}, {0, 0}));
    ASSERT_EQ(std::partial_ordering::greater,
              ChunkVersion({differentEpoch, newerTimestamp}, {3, 1}) <=>
                  ChunkVersion({epoch, timestamp}, {0, 0}));
    ASSERT_EQ(std::partial_ordering::greater,
              ChunkVersion({differentEpoch, newerTimestamp}, {0, 0}) <=>
                  ChunkVersion({epoch, timestamp}, {4, 2}));
    ASSERT_EQ(std::partial_ordering::less,
              ChunkVersion({epoch, timestamp}, {0, 0}) <=>
                  ChunkVersion({differentEpoch, newerTimestamp}, {0, 0}));
    ASSERT_EQ(std::partial_ordering::greater,
              ChunkVersion({differentEpoch, newerTimestamp}, {0, 0}) <=>
                  ChunkVersion({epoch, timestamp}, {0, 0}));
}

}  // namespace
}  // namespace mongo
