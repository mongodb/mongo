// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/logical_time.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/crypto/hash_block.h"
#include "mongo/db/signed_logical_time.h"
#include "mongo/db/time_proof_service.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <cstddef>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

TEST(LogicalTime, comparisons) {
    Timestamp tX(1);
    Timestamp tY(2);

    ASSERT_TRUE(tX < tY);

    ASSERT_TRUE(LogicalTime(tX) < LogicalTime(tY));
    ASSERT_TRUE(LogicalTime(tX) <= LogicalTime(tY));
    ASSERT_TRUE(LogicalTime(tY) > LogicalTime(tX));
    ASSERT_TRUE(LogicalTime(tY) >= LogicalTime(tX));
    ASSERT_TRUE(LogicalTime(tX) != LogicalTime(tY));
    ASSERT_TRUE(LogicalTime(tX) == LogicalTime(tX));

    ASSERT_FALSE(LogicalTime(tX) > LogicalTime(tY));
    ASSERT_FALSE(LogicalTime(tX) >= LogicalTime(tY));
    ASSERT_FALSE(LogicalTime(tY) < LogicalTime(tX));
    ASSERT_FALSE(LogicalTime(tY) <= LogicalTime(tX));
    ASSERT_FALSE(LogicalTime(tX) == LogicalTime(tY));
    ASSERT_FALSE(LogicalTime(tX) != LogicalTime(tX));
}

TEST(LogicalTime, roundtrip) {
    Timestamp tX(1);
    auto tY = LogicalTime(tX).asTimestamp();

    ASSERT_TRUE(tX == tY);
}

TEST(LogicalTime, addTicks) {
    Timestamp tX(1);
    Timestamp tY(2);

    auto lT = LogicalTime(tX);
    lT.addTicks(1);

    ASSERT_TRUE(tY == lT.asTimestamp());
}

TEST(LogicalTime, addTicksConst) {
    Timestamp tX(1);
    Timestamp tY(2);

    const auto lT = LogicalTime(tX);
    const auto lQ = lT.addTicks(1);

    ASSERT_TRUE(tX == lT.asTimestamp());
    ASSERT_TRUE(tY == lQ.asTimestamp());
}

TEST(LogicalTime, defaultInit) {
    Timestamp tX(0);
    LogicalTime lT;
    ASSERT_TRUE(tX == lT.asTimestamp());
}

TEST(LogicalTime, toUnsignedArray) {
    Timestamp tX(123456789);
    auto lT = LogicalTime(tX);

    unsigned char expectedBytes[sizeof(uint64_t)] = {
        0x15, 0xCD, 0x5B, 0x07, 0x00, 0x00, 0x00, 0x00};

    auto unsignedTimeArray = lT.toUnsignedArray();
    for (size_t i = 0; i < sizeof(uint64_t); ++i) {
        ASSERT_EQUALS(unsignedTimeArray[i], expectedBytes[i]);
    }
}

TEST(LogicalTime, appendAsOperationTime) {
    BSONObjBuilder actualResultBuilder;
    LogicalTime testTime(Timestamp(1));
    testTime.appendAsOperationTime(&actualResultBuilder);

    const auto actualResult = actualResultBuilder.obj();
    ASSERT_BSONOBJ_EQ(BSON("operationTime" << Timestamp(1)), actualResult);

    // Test round-trip works
    ASSERT_EQ(testTime, LogicalTime::fromOperationTime(actualResult));
}

TEST(LogicalTime, fromOperationTime) {
    const auto actualTime = LogicalTime::fromOperationTime(
        BSON("someOtherCommandParameter" << "Value"
                                         << "operationTime" << Timestamp(1)));
    ASSERT_EQ(LogicalTime(Timestamp(1)), actualTime);
}

TEST(LogicalTime, fromOperationTimeMissingOperationTime) {
    ASSERT_THROWS_CODE(LogicalTime::fromOperationTime(BSON("someOtherCommandParameter" << "Value")),
                       DBException,
                       ErrorCodes::FailedToParse);
}

TEST(LogicalTime, fromOperationTimeBadType) {
    ASSERT_THROWS_CODE(LogicalTime::fromOperationTime(BSON("operationTime" << "BadStringValue")),
                       DBException,
                       ErrorCodes::BadValue);
}

TEST(SignedLogicalTime, roundtrip) {
    Timestamp tX(1);
    TimeProofService tps;
    TimeProofService::Key key = {};
    auto time = LogicalTime(tX);
    auto proof = tps.getProof(time, key);

    long long keyId = 1;

    SignedLogicalTime signedTime(time, proof, keyId);
    ASSERT_TRUE(time == signedTime.getTime());
    ASSERT_TRUE(proof == signedTime.getProof());
    ASSERT_TRUE(keyId == signedTime.getKeyId());
}

}  // unnamed namespace
}  // namespace mongo
