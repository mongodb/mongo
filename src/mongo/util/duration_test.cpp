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

#include "mongo/stdx/chrono.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"

namespace mongo {
namespace {

// The DurationTestSameType Compare* tests server to check the implementation of the comparison
// operators as well as the compare() method, and so sometimes must explicitly check ASSERT_FALSE(v1
// OP v2). The DurationTestDifferentTypes Compare* tests rely on the fact that the operators all
// just call compare(), so if the operators worked in the SameType tests, they can be trusted in the
// DifferentType tests. As such, the DifferentType tests only use ASSERT_{GT,LT,LTE,GTE,EQ,NE} and
// never do an ASSERT_FALSE.

TEST(DurationComparisonSameType, CompareEqual) {
    ASSERT_EQ(Microseconds::zero(), Microseconds::zero());
    ASSERT_EQ(Microseconds::max(), Microseconds::max());
    ASSERT_EQ(Microseconds::min(), Microseconds::min());
    ASSERT_FALSE(Microseconds::zero() == Microseconds{-1});
}

TEST(DurationComparisonSameType, CompareNotEqual) {
    ASSERT_NE(Microseconds{1}, Microseconds::zero());
    ASSERT_NE(Microseconds{-1}, Microseconds{1});
    ASSERT_FALSE(Microseconds::zero() != Microseconds{0});
}
TEST(DurationComparisonSameType, SameTypeCompareGreaterThan) {
    ASSERT_GT(Microseconds::zero(), Microseconds::min());
    ASSERT_GT(Microseconds{Microseconds::min().count() + 1}, Microseconds::min());
    ASSERT_FALSE(Microseconds{-10} > Microseconds{103});
    ASSERT_FALSE(Microseconds{1} > Microseconds{1});
}

TEST(DurationComparisonSameType, CompareLessThan) {
    ASSERT_LT(Microseconds::zero(), Microseconds::max());
    ASSERT_LT(Microseconds{Microseconds::max().count() - 1}, Microseconds::max());
    ASSERT_LT(Microseconds{1}, Microseconds{10});
    ASSERT_FALSE(Microseconds{1} < Microseconds{1});
    ASSERT_FALSE(Microseconds{-3} < Microseconds{-1200});
}

TEST(DurationComparisonSameType, CompareGreaterThanOrEqual) {
    ASSERT_GTE(Microseconds::zero(), Microseconds::min());
    ASSERT_GTE(Microseconds{Microseconds::min().count() + 1}, Microseconds::min());
    ASSERT_GTE(Microseconds::max(), Microseconds::max());
    ASSERT_GTE(Microseconds::min(), Microseconds::min());
    ASSERT_GTE(Microseconds{5}, Microseconds{5});
    ASSERT_FALSE(Microseconds{-10} > Microseconds{103});
}

TEST(DurationComparisonSameType, CompareLessThanOrEqual) {
    ASSERT_LTE(Microseconds::zero(), Microseconds::max());
    ASSERT_LTE(Microseconds{Microseconds::max().count() - 1}, Microseconds::max());
    ASSERT_LTE(Microseconds{1}, Microseconds{10});
    ASSERT_LTE(Microseconds{1}, Microseconds{1});
    ASSERT_FALSE(Microseconds{-3} < Microseconds{-1200});
}

// Since comparison operators are implemented in terms of Duration::compare, we do not need to
// re-test all of the operators when the duration types are different. It suffices to know that
// compare works, which can be accomplished with EQ, NE and LT alone.

TEST(DurationComparisonDifferentTypes, CompareEqual) {
    ASSERT_EQ(Seconds::zero(), Milliseconds::zero());
    ASSERT_EQ(Seconds{16}, Milliseconds{16000});
    ASSERT_EQ(Minutes{60}, Hours{1});
}
TEST(DurationComparisonDifferentTypes, CompareNotEqual) {
    ASSERT_NE(Milliseconds::max(), Seconds::max());
    ASSERT_NE(Milliseconds::min(), Seconds::min());
    ASSERT_NE(Seconds::max(), Milliseconds::max());
    ASSERT_NE(Seconds::min(), Milliseconds::min());
    ASSERT_NE(Seconds{1}, Milliseconds{1});
}

TEST(DurationComparisonDifferentTypes, CompareLessThan) {
    ASSERT_LT(Milliseconds{1}, Seconds{1});
    ASSERT_LT(Milliseconds{999}, Seconds{1});
    ASSERT_LT(Seconds{1}, Milliseconds{1001});
    ASSERT_LT(Milliseconds{-1001}, Seconds{-1});
    ASSERT_LT(Seconds{-1}, Milliseconds{-1});
    ASSERT_LT(Seconds{-1}, Milliseconds{-999});
}

TEST(DurationComparisonDifferentTypes, CompareAtLimits) {
    ASSERT_LT(Milliseconds::max(), Seconds::max());
    ASSERT_LT(Seconds::min(), Milliseconds::min());

    ASSERT_LT(Milliseconds::min(),
              duration_cast<Milliseconds>(duration_cast<Seconds>(Milliseconds::min())));
    ASSERT_GT(Milliseconds::max(),
              duration_cast<Milliseconds>(duration_cast<Seconds>(Milliseconds::max())));
}

TEST(DurationCast, NonTruncatingDurationCasts) {
    ASSERT_EQ(1, duration_cast<Seconds>(Milliseconds{1000}).count());
    ASSERT_EQ(1000, duration_cast<Milliseconds>(Seconds{1}).count());
    ASSERT_EQ(1000, Milliseconds{Seconds{1}}.count());
    ASSERT_EQ(1053, duration_cast<Milliseconds>(Milliseconds{1053}).count());
}

TEST(DurationCast, TruncatingDurationCasts) {
    ASSERT_EQ(1, duration_cast<Seconds>(Milliseconds{1600}).count());
    ASSERT_EQ(0, duration_cast<Seconds>(Milliseconds{999}).count());
    ASSERT_EQ(-1, duration_cast<Seconds>(Milliseconds{-1600}).count());
    ASSERT_EQ(0, duration_cast<Seconds>(Milliseconds{-999}).count());
}

TEST(DurationCast, OverflowingCastsThrow) {
    ASSERT_THROWS_CODE(
        duration_cast<Milliseconds>(Seconds::max()), UserException, ErrorCodes::DurationOverflow);
    ASSERT_THROWS_CODE(
        duration_cast<Milliseconds>(Seconds::min()), UserException, ErrorCodes::DurationOverflow);
}

TEST(DurationCast, ImplicitConversionToStdxDuration) {
    auto standardMillis = Milliseconds{10}.toSystemDuration();
    ASSERT_EQUALS(Milliseconds{10}, duration_cast<Milliseconds>(standardMillis));
}

TEST(DurationAssignment, DurationAssignment) {
    Milliseconds ms = Milliseconds{15};
    Milliseconds ms2 = ms;
    Milliseconds ms3 = Milliseconds{30};
    ms3 = ms;
    ASSERT_EQ(ms, ms3);
    ASSERT_EQ(ms2, ms3);
}

TEST(DurationArithmetic, AddNoOverflowSucceeds) {
    ASSERT_EQ(Milliseconds{1001}, Milliseconds{1} + Seconds{1});
    ASSERT_EQ(Milliseconds{1001}, Seconds{1} + Milliseconds{1});
    ASSERT_EQ(Milliseconds{1001}, Milliseconds{1} + Milliseconds{1000});
}

TEST(DurationArithmetic, AddOverflowThrows) {
    // Max + 1 should throw
    ASSERT_THROWS_CODE(
        Milliseconds::max() + Milliseconds{1}, UserException, ErrorCodes::DurationOverflow);

    // Min + -1 should throw
    ASSERT_THROWS_CODE(
        Milliseconds::min() + Milliseconds{-1}, UserException, ErrorCodes::DurationOverflow);

    // Conversion of Seconds::min() to Milliseconds should throw
    ASSERT_THROWS_CODE(
        Seconds::min() + Milliseconds{1}, UserException, ErrorCodes::DurationOverflow);
    ASSERT_THROWS_CODE(
        Milliseconds{1} + Seconds::min(), UserException, ErrorCodes::DurationOverflow);
}

TEST(DurationArithmetic, SubtractNoOverflowSucceeds) {
    ASSERT_EQ(Milliseconds{-999}, Milliseconds{1} - Seconds{1});
    ASSERT_EQ(Milliseconds{999}, Seconds{1} - Milliseconds{1});
    ASSERT_EQ(Milliseconds{-999}, Milliseconds{1} - Milliseconds{1000});
    ASSERT_EQ(Milliseconds::zero() - Milliseconds{1}, -Milliseconds{1});
}

TEST(DurationArithmetic, SubtractOverflowThrows) {
    // Min - 1 should throw
    ASSERT_THROWS_CODE(
        Milliseconds::min() - Milliseconds{1}, UserException, ErrorCodes::DurationOverflow);

    // Max + -1 should throw
    ASSERT_THROWS_CODE(
        Milliseconds::max() - Milliseconds{-1}, UserException, ErrorCodes::DurationOverflow);

    // Conversion of Seconds::min() to Milliseconds should throw
    ASSERT_THROWS_CODE(
        Seconds::min() - Milliseconds{1}, UserException, ErrorCodes::DurationOverflow);
    ASSERT_THROWS_CODE(
        Milliseconds{1} - Seconds::min(), UserException, ErrorCodes::DurationOverflow);
}

TEST(DurationArithmetic, MultiplyNoOverflowSucceds) {
    ASSERT_EQ(Milliseconds{150}, 15 * Milliseconds{10});
    ASSERT_EQ(Milliseconds{150}, Milliseconds{15} * 10);
}

TEST(DurationArithmetic, MultilpyOverflowThrows) {
    ASSERT_THROWS_CODE(Milliseconds::max() * 2, UserException, ErrorCodes::DurationOverflow);
    ASSERT_THROWS_CODE(2 * Milliseconds::max(), UserException, ErrorCodes::DurationOverflow);
    ASSERT_THROWS_CODE(Milliseconds::max() * -2, UserException, ErrorCodes::DurationOverflow);
    ASSERT_THROWS_CODE(-2 * Milliseconds::max(), UserException, ErrorCodes::DurationOverflow);
}

TEST(DurationArithmetic, DivideNoOverflowSucceeds) {
    ASSERT_EQ(Milliseconds{-1}, Milliseconds{2} / -2);
}

TEST(DurationArithmetic, DivideOverflowThrows) {
    ASSERT_THROWS_CODE(Milliseconds::min() / -1, UserException, ErrorCodes::DurationOverflow);
}

}  // namespace
}  // namespace mongo
