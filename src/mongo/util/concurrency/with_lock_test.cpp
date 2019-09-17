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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/platform/mutex.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/log.h"

#include <iostream>

namespace mongo {

namespace {

struct Beerp {
    explicit Beerp(int i) {
        _blerp(WithLock::withoutLock(), i);
    }
    Beerp(stdx::lock_guard<Latch> const& lk, int i) {
        _blerp(lk, i);
    }
    int bleep(char n) {
        stdx::lock_guard<Latch> lk(_m);
        return _bloop(lk, n - '0');
    }
    int bleep(int i) {
        stdx::unique_lock<Latch> lk(_m);
        return _bloop(lk, i);
    }

private:
    int _bloop(WithLock lk, int i) {
        return _blerp(lk, i);
    }
    int _blerp(WithLock, int i) {
        log() << i << " bleep" << (i == 1 ? "\n" : "s\n");
        return i;
    }
    Mutex _m = MONGO_MAKE_LATCH("Beerp::_m");
};

TEST(WithLockTest, OverloadSet) {
    Beerp b(0);
    ASSERT_EQ(1, b.bleep('1'));
    ASSERT_EQ(2, b.bleep(2));

    auto m = MONGO_MAKE_LATCH();
    stdx::lock_guard<Latch> lk(m);
    Beerp(lk, 3);
}

}  // namespace
}  // namespace mongo
