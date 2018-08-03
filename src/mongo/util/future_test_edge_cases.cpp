/**
 *    Copyright (C) 2018 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/util/future.h"

#include "mongo/stdx/thread.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include "mongo/util/future_test_utils.h"

namespace mongo {
namespace {

// This is the motivating case for SharedStateBase::isJustForContinuation. Without that logic, there
// would be a long chain of SharedStates, growing longer with each recursion. That logic exists to
// limit it to a fixed-size chain.
TEST(Future_EdgeCases, looping_onError) {
    int tries = 10;
    std::function<Future<int>()> read = [&] {
        return async([&] {
                   uassert(ErrorCodes::BadValue, "", --tries == 0);
                   return tries;
               })
            .onError([&](Status) { return read(); });
    };
    ASSERT_EQ(read().get(), 0);
}

// This tests for a bug in an earlier implementation of isJustForContinuation. Due to an off-by-one,
// it would replace the "then" continuation's SharedState. A different type is used for the return
// from then to cause it to fail a checked_cast close to the bug in debug builds.
TEST(Future_EdgeCases, looping_onError_with_then) {
    int tries = 10;
    std::function<Future<int>()> read = [&] {
        return async([&] {
                   uassert(ErrorCodes::BadValue, "", --tries == 0);
                   return tries;
               })
            .onError([&](Status) { return read(); });
    };
    ASSERT_EQ(read().then([](int x) { return x + 0.5; }).get(), 0.5);
}

// Make sure we actually die if someone throws from the getAsync callback.
//
// With gcc 5.8 we terminate, but print "terminate() called. No exception is active". This works in
// clang and gcc 7, so hopefully we can change the death-test search string to "die die die!!!" when
// we upgrade the toolchain.
DEATH_TEST(Future_EdgeCases, Success_getAsync_throw, "terminate() called") {
    Future<void>::makeReady().getAsync(
        [](Status) { uasserted(ErrorCodes::BadValue, "die die die!!!"); });
}

}  // namespace
}  // namespace mongo
