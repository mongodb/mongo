/**
 *    Copyright (C) 2017 MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#include "mongo/db/logical_time.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/signed_logical_time.h"
#include "mongo/db/time_proof_service.h"
#include "mongo/platform/basic.h"
#include "mongo/unittest/unittest.h"

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

TEST(SignedLogicalTime, roundtrip) {
    Timestamp tX(1);
    std::array<std::uint8_t, 20> tempKey = {};
    TimeProofService::Key key(std::move(tempKey));
    TimeProofService tps(std::move(key));
    auto time = LogicalTime(tX);
    auto proof = tps.getProof(time);

    SignedLogicalTime signedTime(time, proof);
    ASSERT_TRUE(time == signedTime.getTime());
    ASSERT_TRUE(proof == signedTime.getProof());
}

}  // unnamed namespace
}  // namespace mongo
