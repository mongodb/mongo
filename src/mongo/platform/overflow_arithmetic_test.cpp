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


#define assertArithOverflow(TYPE, FN, LHS, RHS, EXPECT_OVERFLOW, EXPECTED_RESULT)            \
    do {                                                                                     \
        const bool expectOverflow = EXPECT_OVERFLOW;                                         \
        TYPE result;                                                                         \
        ASSERT_EQ(expectOverflow, FN(LHS, RHS, &result)) << #FN "(" #LHS ", " #RHS;          \
        if (!expectOverflow) {                                                               \
            ASSERT_EQ(TYPE(EXPECTED_RESULT), TYPE(result)) << #FN "(" #LHS ", " #RHS " - >"; \
        }                                                                                    \
    } while (false)

#define assertSignedMultiplyNoOverflow(LHS, RHS, EXPECTED) \
    assertArithOverflow(int64_t, mongoSignedMultiplyOverflow64, LHS, RHS, false, EXPECTED)
#define assertSignedMultiplyWithOverflow(LHS, RHS) \
    assertArithOverflow(int64_t, mongoSignedMultiplyOverflow64, LHS, RHS, true, 0)

#define assertUnsignedMultiplyNoOverflow(LHS, RHS, EXPECTED) \
    assertArithOverflow(uint64_t, mongoUnsignedMultiplyOverflow64, LHS, RHS, false, EXPECTED)
#define assertUnsignedMultiplyWithOverflow(LHS, RHS) \
    assertArithOverflow(uint64_t, mongoUnsignedMultiplyOverflow64, LHS, RHS, true, 0)

#define assertSignedAddNoOverflow(LHS, RHS, EXPECTED) \
    assertArithOverflow(int64_t, mongoSignedAddOverflow64, LHS, RHS, false, EXPECTED)
#define assertSignedAddWithOverflow(LHS, RHS) \
    assertArithOverflow(int64_t, mongoSignedAddOverflow64, LHS, RHS, true, 0)

#define assertUnsignedAddNoOverflow(LHS, RHS, EXPECTED) \
    assertArithOverflow(uint64_t, mongoUnsignedAddOverflow64, LHS, RHS, false, EXPECTED)
#define assertUnsignedAddWithOverflow(LHS, RHS) \
    assertArithOverflow(uint64_t, mongoUnsignedAddOverflow64, LHS, RHS, true, 0)

#define assertSignedSubtractNoOverflow(LHS, RHS, EXPECTED) \
    assertArithOverflow(int64_t, mongoSignedSubtractOverflow64, LHS, RHS, false, EXPECTED)
#define assertSignedSubtractWithOverflow(LHS, RHS) \
    assertArithOverflow(int64_t, mongoSignedSubtractOverflow64, LHS, RHS, true, 0)

#define assertUnsignedSubtractNoOverflow(LHS, RHS, EXPECTED) \
    assertArithOverflow(uint64_t, mongoUnsignedSubtractOverflow64, LHS, RHS, false, EXPECTED)
#define assertUnsignedSubtractWithOverflow(LHS, RHS) \
    assertArithOverflow(uint64_t, mongoUnsignedSubtractOverflow64, LHS, RHS, true, 0)

TEST(OverflowArithmetic, SignedMultiplicationTests) {
    using limits = std::numeric_limits<int64_t>;
    assertSignedMultiplyNoOverflow(0, limits::max(), 0);
    assertSignedMultiplyNoOverflow(0, limits::min(), 0);
    assertSignedMultiplyNoOverflow(1, limits::max(), limits::max());
    assertSignedMultiplyNoOverflow(1, limits::min(), limits::min());
    assertSignedMultiplyNoOverflow(-1, limits::max(), limits::min() + 1);
    assertSignedMultiplyNoOverflow(1000, 57, 57000);
    assertSignedMultiplyNoOverflow(1000, -57, -57000);
    assertSignedMultiplyNoOverflow(-1000, -57, 57000);
    assertSignedMultiplyNoOverflow(0x3fffffffffffffff, 2, 0x7ffffffffffffffe);
    assertSignedMultiplyNoOverflow(0x3fffffffffffffff, -2, -0x7ffffffffffffffe);
    assertSignedMultiplyNoOverflow(-0x3fffffffffffffff, -2, 0x7ffffffffffffffe);

    assertSignedMultiplyWithOverflow(-1, limits::min());
    assertSignedMultiplyWithOverflow(2, limits::max());
    assertSignedMultiplyWithOverflow(-2, limits::max());
    assertSignedMultiplyWithOverflow(2, limits::min());
    assertSignedMultiplyWithOverflow(-2, limits::min());
    assertSignedMultiplyWithOverflow(limits::min(), limits::max());
    assertSignedMultiplyWithOverflow(limits::max(), limits::max());
    assertSignedMultiplyWithOverflow(limits::min(), limits::min());
    assertSignedMultiplyWithOverflow(1LL << 62, 8);
    assertSignedMultiplyWithOverflow(-(1LL << 62), 8);
    assertSignedMultiplyWithOverflow(-(1LL << 62), -8);
}

TEST(OverflowArithmetic, UnignedMultiplicationTests) {
    using limits = std::numeric_limits<uint64_t>;
    assertUnsignedMultiplyNoOverflow(0, limits::max(), 0);
    assertUnsignedMultiplyNoOverflow(1, limits::max(), limits::max());
    assertUnsignedMultiplyNoOverflow(1000, 57, 57000);
    assertUnsignedMultiplyNoOverflow(0x3fffffffffffffff, 2, 0x7ffffffffffffffe);
    assertUnsignedMultiplyNoOverflow(0x7fffffffffffffff, 2, 0xfffffffffffffffe);

    assertUnsignedMultiplyWithOverflow(2, limits::max());
    assertUnsignedMultiplyWithOverflow(limits::max(), limits::max());
    assertUnsignedMultiplyWithOverflow(1LL << 62, 8);
    assertUnsignedMultiplyWithOverflow(0x7fffffffffffffff, 4);
}

TEST(OverflowArithmetic, SignedAdditionTests) {
    using limits = std::numeric_limits<int64_t>;
    assertSignedAddNoOverflow(0, limits::max(), limits::max());
    assertSignedAddNoOverflow(-1, limits::max(), limits::max() - 1);
    assertSignedAddNoOverflow(1, limits::max() - 1, limits::max());
    assertSignedAddNoOverflow(0, limits::min(), limits::min());
    assertSignedAddNoOverflow(1, limits::min(), limits::min() + 1);
    assertSignedAddNoOverflow(-1, limits::min() + 1, limits::min());
    assertSignedAddNoOverflow(limits::max(), limits::min(), -1);
    assertSignedAddNoOverflow(1, 1, 2);
    assertSignedAddNoOverflow(-1, -1, -2);

    assertSignedAddWithOverflow(limits::max(), 1);
    assertSignedAddWithOverflow(limits::max(), limits::max());
    assertSignedAddWithOverflow(limits::min(), -1);
    assertSignedAddWithOverflow(limits::min(), limits::min());
}

TEST(OverflowArithmetic, UnsignedAdditionTests) {
    using limits = std::numeric_limits<uint64_t>;
    assertUnsignedAddNoOverflow(0, limits::max(), limits::max());
    assertUnsignedAddNoOverflow(1, limits::max() - 1, limits::max());
    assertUnsignedAddNoOverflow(1, 1, 2);

    assertUnsignedAddWithOverflow(limits::max(), 1);
    assertUnsignedAddWithOverflow(limits::max(), limits::max());
}

TEST(OverflowArithmetic, SignedSubtractionTests) {
    using limits = std::numeric_limits<int64_t>;
    assertSignedSubtractNoOverflow(limits::max(), 0, limits::max());
    assertSignedSubtractNoOverflow(limits::max(), 1, limits::max() - 1);
    assertSignedSubtractNoOverflow(limits::max() - 1, -1, limits::max());
    assertSignedSubtractNoOverflow(limits::min(), 0, limits::min());
    assertSignedSubtractNoOverflow(limits::min(), -1, limits::min() + 1);
    assertSignedSubtractNoOverflow(limits::min() + 1, 1, limits::min());
    assertSignedSubtractNoOverflow(limits::max(), limits::max(), 0);
    assertSignedSubtractNoOverflow(limits::min(), limits::min(), 0);
    assertSignedSubtractNoOverflow(0, 0, 0);
    assertSignedSubtractNoOverflow(1, 1, 0);
    assertSignedSubtractNoOverflow(0, 1, -1);

    assertSignedSubtractWithOverflow(0, limits::min());
    assertSignedSubtractWithOverflow(limits::max(), -1);
    assertSignedSubtractWithOverflow(limits::max(), limits::min());
    assertSignedSubtractWithOverflow(limits::min(), 1);
    assertSignedSubtractWithOverflow(limits::min(), limits::max());
}

TEST(OverflowArithmetic, UnsignedSubtractionTests) {
    using limits = std::numeric_limits<uint64_t>;
    assertUnsignedSubtractNoOverflow(limits::max(), 0, limits::max());
    assertUnsignedSubtractNoOverflow(limits::max(), 1, limits::max() - 1);
    assertUnsignedSubtractNoOverflow(limits::max(), limits::max(), 0);
    assertUnsignedSubtractNoOverflow(0, 0, 0);
    assertUnsignedSubtractNoOverflow(1, 1, 0);

    assertUnsignedSubtractWithOverflow(0, 1);
    assertUnsignedSubtractWithOverflow(0, limits::max());
}

}  // namespace
}  // namespace mongo
