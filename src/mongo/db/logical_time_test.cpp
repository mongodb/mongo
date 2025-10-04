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
