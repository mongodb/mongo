/*    Copyright 2016 MongoDB, Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include <limits>

#include "mongo/platform/overflow_arithmetic.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using limits = std::numeric_limits<int64_t>;

#define assertArithOverflow(FN, LHS, RHS, EXPECT_OVERFLOW, EXPECTED_RESULT)         \
    do {                                                                            \
        const bool expectOverflow = EXPECT_OVERFLOW;                                \
        int64_t result;                                                             \
        ASSERT_EQ(expectOverflow, FN(LHS, RHS, &result)) << #FN "(" #LHS ", " #RHS; \
        if (!expectOverflow) {                                                      \
            ASSERT_EQ(EXPECTED_RESULT, result) << #FN "(" #LHS ", " #RHS " - >";    \
        }                                                                           \
    } while (false)

#define assertMultiplyNoOverflow(LHS, RHS, EXPECTED) \
    assertArithOverflow(mongoSignedMultiplyOverflow64, LHS, RHS, false, EXPECTED)
#define assertMultiplyWithOverflow(LHS, RHS) \
    assertArithOverflow(mongoSignedMultiplyOverflow64, LHS, RHS, true, 0)

#define assertAddNoOverflow(LHS, RHS, EXPECTED) \
    assertArithOverflow(mongoSignedAddOverflow64, LHS, RHS, false, EXPECTED)
#define assertAddWithOverflow(LHS, RHS) \
    assertArithOverflow(mongoSignedAddOverflow64, LHS, RHS, true, 0)

#define assertSubtractNoOverflow(LHS, RHS, EXPECTED) \
    assertArithOverflow(mongoSignedSubtractOverflow64, LHS, RHS, false, EXPECTED)
#define assertSubtractWithOverflow(LHS, RHS) \
    assertArithOverflow(mongoSignedSubtractOverflow64, LHS, RHS, true, 0)

TEST(OverflowArithmetic, MultiplicationTests) {
    assertMultiplyNoOverflow(0, limits::max(), 0);
    assertMultiplyNoOverflow(0, limits::min(), 0);
    assertMultiplyNoOverflow(1, limits::max(), limits::max());
    assertMultiplyNoOverflow(1, limits::min(), limits::min());
    assertMultiplyNoOverflow(-1, limits::max(), limits::min() + 1);
    assertMultiplyNoOverflow(1000, 57, 57000);
    assertMultiplyNoOverflow(1000, -57, -57000);
    assertMultiplyNoOverflow(-1000, -57, 57000);
    assertMultiplyNoOverflow(0x3fffffffffffffff, 2, 0x7ffffffffffffffe);
    assertMultiplyNoOverflow(0x3fffffffffffffff, -2, -0x7ffffffffffffffe);
    assertMultiplyNoOverflow(-0x3fffffffffffffff, -2, 0x7ffffffffffffffe);

    assertMultiplyWithOverflow(-1, limits::min());
    assertMultiplyWithOverflow(2, limits::max());
    assertMultiplyWithOverflow(-2, limits::max());
    assertMultiplyWithOverflow(2, limits::min());
    assertMultiplyWithOverflow(-2, limits::min());
    assertMultiplyWithOverflow(limits::min(), limits::max());
    assertMultiplyWithOverflow(limits::max(), limits::max());
    assertMultiplyWithOverflow(limits::min(), limits::min());
    assertMultiplyWithOverflow(1LL << 62, 8);
    assertMultiplyWithOverflow(-(1LL << 62), 8);
    assertMultiplyWithOverflow(-(1LL << 62), -8);
}

TEST(OverflowArithmetic, AdditionTests) {
    assertAddNoOverflow(0, limits::max(), limits::max());
    assertAddNoOverflow(-1, limits::max(), limits::max() - 1);
    assertAddNoOverflow(0, limits::min(), limits::min());
    assertAddNoOverflow(1, limits::min(), limits::min() + 1);
    assertAddNoOverflow(limits::max(), limits::min(), -1);
    assertAddNoOverflow(1, 1, 2);
    assertAddNoOverflow(-1, -1, -2);

    assertAddWithOverflow(limits::max(), 1);
    assertAddWithOverflow(limits::max(), limits::max());
    assertAddWithOverflow(limits::min(), -1);
    assertAddWithOverflow(limits::min(), limits::min());
}

TEST(OverflowArithmetic, SubtractionTests) {
    assertSubtractNoOverflow(limits::max(), 0, limits::max());
    assertSubtractNoOverflow(limits::max(), 1, limits::max() - 1);
    assertSubtractNoOverflow(limits::min(), 0, limits::min());
    assertSubtractNoOverflow(limits::min(), -1, limits::min() + 1);
    assertSubtractNoOverflow(limits::max(), limits::max(), 0);
    assertSubtractNoOverflow(limits::min(), limits::min(), 0);

    assertSubtractWithOverflow(0, limits::min());
    assertSubtractWithOverflow(limits::max(), -1);
    assertSubtractWithOverflow(limits::max(), limits::min());
    assertSubtractWithOverflow(limits::min(), 1);
    assertSubtractWithOverflow(limits::min(), limits::max());
}

}  // namespace
}  // namespace mongo
