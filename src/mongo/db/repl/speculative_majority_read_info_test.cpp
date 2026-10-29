// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/speculative_majority_read_info.h"

#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <string>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace repl {
namespace {

TEST(SpeculativeMajorityReadInfo, NonSpeculativeRead) {
    SpeculativeMajorityReadInfo readInfo;
    ASSERT_FALSE(readInfo.isSpeculativeRead());
}

DEATH_TEST(SpeculativeMajorityReadInfoDeathTest,
           NonSpeculativeReadCannotRetrieveOpTime,
           "Invariant failure") {
    SpeculativeMajorityReadInfo readInfo;
    ASSERT_FALSE(readInfo.isSpeculativeRead());
    readInfo.getSpeculativeReadTimestamp();
}

TEST(SpeculativeMajorityReadInfo, SetSpeculativeRead) {
    SpeculativeMajorityReadInfo readInfo;
    readInfo.setIsSpeculativeRead();
    ASSERT(readInfo.isSpeculativeRead());
    ASSERT_FALSE(readInfo.getSpeculativeReadTimestamp());
}

TEST(SpeculativeMajorityReadInfo, SetSpeculativeReadOpTime) {
    SpeculativeMajorityReadInfo readInfo;
    readInfo.setIsSpeculativeRead();
    readInfo.setSpeculativeReadTimestampForward(Timestamp(1, 0));
    auto readTs = readInfo.getSpeculativeReadTimestamp();
    ASSERT(readInfo.isSpeculativeRead());
    ASSERT(readTs);
    ASSERT_EQ(*readTs, Timestamp(1, 0));
}

DEATH_TEST(SpeculativeMajorityReadInfoDeathTest,
           CannotSetSpeculativeReadOpTimeOnNonSpeculativeRead,
           "Invariant failure") {
    SpeculativeMajorityReadInfo readInfo;
    readInfo.setSpeculativeReadTimestampForward(Timestamp(1, 0));
}

TEST(SpeculativeMajorityReadInfo, SpeculativeReadOpTimesCanMonotonicallyIncrease) {
    SpeculativeMajorityReadInfo readInfo;
    readInfo.setIsSpeculativeRead();

    // Set the initial read timestamp.
    readInfo.setSpeculativeReadTimestampForward(Timestamp(1, 0));
    auto readTs = readInfo.getSpeculativeReadTimestamp();
    ASSERT(readInfo.isSpeculativeRead());
    ASSERT(readTs);

    // Allowed to keep the timestamp the same.
    readInfo.setSpeculativeReadTimestampForward(Timestamp(1, 0));
    readTs = readInfo.getSpeculativeReadTimestamp();
    ASSERT(readTs);
    ASSERT_EQ(*readTs, Timestamp(1, 0));

    // Allowed to increase the timestamp.
    readInfo.setSpeculativeReadTimestampForward(Timestamp(2, 0));
    readTs = readInfo.getSpeculativeReadTimestamp();
    ASSERT(readTs);
    ASSERT_EQ(*readTs, Timestamp(2, 0));
}

TEST(SpeculativeMajorityReadInfo, SpeculativeReadOpTimeCannotDecrease) {
    SpeculativeMajorityReadInfo readInfo;
    readInfo.setIsSpeculativeRead();
    readInfo.setSpeculativeReadTimestampForward(Timestamp(2, 0));
    auto readTs = readInfo.getSpeculativeReadTimestamp();
    ASSERT(readInfo.isSpeculativeRead());
    ASSERT(readTs);

    // Timestamp cannot decrease.
    readInfo.setSpeculativeReadTimestampForward(Timestamp(1, 0));

    ASSERT(readInfo.isSpeculativeRead());
    ASSERT(readTs);
    ASSERT_EQ(*readTs, Timestamp(2, 0));
}

TEST(SpeculativeMajorityReadInfo, SetSpeculativeReadIsIdempotent) {
    SpeculativeMajorityReadInfo readInfo;

    // Once a read has been marked speculative, trying to mark it again as such should have no
    // effect.
    readInfo.setIsSpeculativeRead();
    ASSERT(readInfo.isSpeculativeRead());
    ASSERT_FALSE(readInfo.getSpeculativeReadTimestamp());

    // Should have no effect.
    readInfo.setIsSpeculativeRead();
    ASSERT(readInfo.isSpeculativeRead());
    ASSERT_FALSE(readInfo.getSpeculativeReadTimestamp());

    readInfo.setSpeculativeReadTimestampForward(Timestamp(1, 0));
    ASSERT(readInfo.isSpeculativeRead());
    ASSERT(readInfo.getSpeculativeReadTimestamp());

    // Should have no effect.
    readInfo.setIsSpeculativeRead();
    ASSERT(readInfo.isSpeculativeRead());
    ASSERT(readInfo.getSpeculativeReadTimestamp());
}

}  // unnamed namespace
}  // namespace repl
}  // namespace mongo
