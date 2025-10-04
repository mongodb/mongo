/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/util/strong_weak_finish_line.h"

#include "mongo/base/string_data.h"
#include "mongo/unittest/unittest.h"

#include <memory>

namespace mongo {
namespace {

TEST(StrongWeakFinishLineTest, WeakArrivalFollowedByStrong) {
    StrongWeakFinishLine finishLine(100);

    ASSERT_FALSE(finishLine.isReady());

    ASSERT_FALSE(finishLine.arriveWeakly());
    ASSERT_FALSE(finishLine.isReady());

    ASSERT_FALSE(finishLine.arriveWeakly());
    ASSERT_FALSE(finishLine.isReady());

    ASSERT(finishLine.arriveStrongly());
    ASSERT(finishLine.isReady());

    ASSERT_FALSE(finishLine.arriveStrongly());
    ASSERT_FALSE(finishLine.arriveWeakly());
}

TEST(StrongWeakFinishLineTest, AllWeakArrival) {
    StrongWeakFinishLine finishLine(3);

    ASSERT_FALSE(finishLine.isReady());

    ASSERT_FALSE(finishLine.arriveWeakly());
    ASSERT_FALSE(finishLine.isReady());

    ASSERT_FALSE(finishLine.arriveWeakly());
    ASSERT_FALSE(finishLine.isReady());

    ASSERT(finishLine.arriveWeakly());
    ASSERT(finishLine.isReady());
}

TEST(StrongWeakFinishLineTest, LastWeakArrivalAfterStrongReturnsFalse) {
    StrongWeakFinishLine finishLine(3);

    ASSERT_FALSE(finishLine.isReady());

    ASSERT_FALSE(finishLine.arriveWeakly());
    ASSERT_FALSE(finishLine.isReady());

    ASSERT(finishLine.arriveStrongly());
    ASSERT(finishLine.isReady());

    ASSERT_FALSE(finishLine.arriveWeakly());
}

}  // namespace
}  // namespace mongo
