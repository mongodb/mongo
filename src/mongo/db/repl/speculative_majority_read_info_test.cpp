
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

#include "mongo/db/repl/speculative_majority_read_info.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace repl {
namespace {

TEST(SpeculativeMajorityReadInfo, NonSpeculativeRead) {
    SpeculativeMajorityReadInfo readInfo;
    ASSERT_FALSE(readInfo.isSpeculativeRead());
}

DEATH_TEST(SpeculativeMajorityReadInfo,
           NonSpeculativeReadCannotRetrieveOpTime,
           "Invariant failure") {
    SpeculativeMajorityReadInfo readInfo;
    ASSERT_FALSE(readInfo.isSpeculativeRead());
    readInfo.getSpeculativeReadOpTime();
}

TEST(SpeculativeMajorityReadInfo, SetSpeculativeRead) {
    SpeculativeMajorityReadInfo readInfo;
    readInfo.setIsSpeculativeRead();
    ASSERT(readInfo.isSpeculativeRead());
    ASSERT_FALSE(readInfo.getSpeculativeReadOpTime());
}

TEST(SpeculativeMajorityReadInfo, SetSpeculativeReadOpTime) {
    SpeculativeMajorityReadInfo readInfo;
    readInfo.setIsSpeculativeRead();
    readInfo.setSpeculativeReadOpTimeForward(OpTime(Timestamp(1, 0), 1));
    auto readOpTime = readInfo.getSpeculativeReadOpTime();
    ASSERT(readInfo.isSpeculativeRead());
    ASSERT(readOpTime);
    ASSERT_EQ(*readOpTime, OpTime(Timestamp(1, 0), 1));
}

DEATH_TEST(SpeculativeMajorityReadInfo,
           CannotSetSpeculativeReadOpTimeOnNonSpeculativeRead,
           "Invariant failure") {
    SpeculativeMajorityReadInfo readInfo;
    readInfo.setSpeculativeReadOpTimeForward(OpTime(Timestamp(1, 0), 1));
}

TEST(SpeculativeMajorityReadInfo, SpeculativeReadOpTimesCanMonotonicallyIncrease) {
    SpeculativeMajorityReadInfo readInfo;
    readInfo.setIsSpeculativeRead();

    // Set the initial read optime.
    readInfo.setSpeculativeReadOpTimeForward(OpTime(Timestamp(1, 0), 1));
    auto readOpTime = readInfo.getSpeculativeReadOpTime();
    ASSERT(readInfo.isSpeculativeRead());
    ASSERT(readOpTime);

    // Allowed to keep the optime the same.
    readInfo.setSpeculativeReadOpTimeForward(OpTime(Timestamp(1, 0), 1));
    readOpTime = readInfo.getSpeculativeReadOpTime();
    ASSERT(readOpTime);
    ASSERT_EQ(*readOpTime, OpTime(Timestamp(1, 0), 1));

    // Allowed to increase the optime.
    readInfo.setSpeculativeReadOpTimeForward(OpTime(Timestamp(2, 0), 1));
    readOpTime = readInfo.getSpeculativeReadOpTime();
    ASSERT(readOpTime);
    ASSERT_EQ(*readOpTime, OpTime(Timestamp(2, 0), 1));
}

TEST(SpeculativeMajorityReadInfo, SpeculativeReadOpTimeCannotDecrease) {
    SpeculativeMajorityReadInfo readInfo;
    readInfo.setIsSpeculativeRead();
    readInfo.setSpeculativeReadOpTimeForward(OpTime(Timestamp(2, 0), 1));
    auto readOpTime = readInfo.getSpeculativeReadOpTime();
    ASSERT(readInfo.isSpeculativeRead());
    ASSERT(readOpTime);

    // Optime cannot decrease.
    readInfo.setSpeculativeReadOpTimeForward(OpTime(Timestamp(1, 0), 1));

    ASSERT(readInfo.isSpeculativeRead());
    ASSERT(readOpTime);
    ASSERT_EQ(*readOpTime, OpTime(Timestamp(2, 0), 1));
}

TEST(SpeculativeMajorityReadInfo, SetSpeculativeReadIsIdempotent) {
    SpeculativeMajorityReadInfo readInfo;

    // Once a read has been marked speculative, trying to mark it again as such should have no
    // effect.
    readInfo.setIsSpeculativeRead();
    ASSERT(readInfo.isSpeculativeRead());
    ASSERT_FALSE(readInfo.getSpeculativeReadOpTime());

    // Should have no effect.
    readInfo.setIsSpeculativeRead();
    ASSERT(readInfo.isSpeculativeRead());
    ASSERT_FALSE(readInfo.getSpeculativeReadOpTime());

    readInfo.setSpeculativeReadOpTimeForward(OpTime(Timestamp(1, 0), 1));
    ASSERT(readInfo.isSpeculativeRead());
    ASSERT(readInfo.getSpeculativeReadOpTime());

    // Should have no effect.
    readInfo.setIsSpeculativeRead();
    ASSERT(readInfo.isSpeculativeRead());
    ASSERT(readInfo.getSpeculativeReadOpTime());
}

}  // unnamed namespace
}  // namespace repl
}  // namespace mongo
