/**
 *    Copyright (C) 2017 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  fmodify
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
 *    all of the code used other than as permitted herein. If you fmodify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/matcher/schema/expression_internal_schema_fmod.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(InternalSchemaFmodMatchExpression, MatchesElement) {
    BSONObj match = BSON("a" << 1);
    BSONObj largerMatch = BSON("a" << 4.0);
    BSONObj longLongMatch = BSON("a" << 68719476736LL);
    BSONObj notMatch = BSON("a" << 6);
    BSONObj negativeNotMatch = BSON("a" << -2);
    InternalSchemaFmodMatchExpression fmod;
    ASSERT_OK(fmod.init("", Decimal128(3), Decimal128(1)));
    ASSERT_TRUE(fmod.matchesSingleElement(match.firstElement()));
    ASSERT_TRUE(fmod.matchesSingleElement(largerMatch.firstElement()));
    ASSERT_TRUE(fmod.matchesSingleElement(longLongMatch.firstElement()));
    ASSERT_FALSE(fmod.matchesSingleElement(notMatch.firstElement()));
    ASSERT_FALSE(fmod.matchesSingleElement(negativeNotMatch.firstElement()));
}

TEST(InternalSchemaFmodMatchExpression, ZeroDivisor) {
    InternalSchemaFmodMatchExpression fmod;
    ASSERT_NOT_OK(fmod.init("", Decimal128(0), Decimal128(1)));
}

TEST(InternalSchemaFmodMatchExpression, MatchesScalar) {
    InternalSchemaFmodMatchExpression fmod;
    ASSERT_OK(fmod.init("a", Decimal128(5), Decimal128(2)));
    ASSERT_TRUE(fmod.matchesBSON(BSON("a" << 7.0)));
    ASSERT_FALSE(fmod.matchesBSON(BSON("a" << 4)));
}

TEST(InternalSchemaFmodMatchExpression, MatchesNonIntegralValue) {
    InternalSchemaFmodMatchExpression fmod;
    ASSERT_OK(fmod.init("a", Decimal128(10.5), Decimal128((4.5))));
    ASSERT_TRUE(fmod.matchesBSON(BSON("a" << 15.0)));
    ASSERT_FALSE(fmod.matchesBSON(BSON("a" << 10.0)));
}

TEST(InternalSchemaFmodMatchExpression, MatchesArrayValue) {
    InternalSchemaFmodMatchExpression fmod;
    ASSERT_OK(fmod.init("a", Decimal128(5), Decimal128(2)));
    ASSERT_TRUE(fmod.matchesBSON(BSON("a" << BSON_ARRAY(5 << 12LL))));
    ASSERT_FALSE(fmod.matchesBSON(BSON("a" << BSON_ARRAY(6 << 8))));
}

TEST(InternalSchemaFmodMatchExpression, DoesNotMatchNull) {
    InternalSchemaFmodMatchExpression fmod;
    ASSERT_OK(fmod.init("a", Decimal128(5), Decimal128(2)));
    ASSERT_FALSE(fmod.matchesBSON(BSONObj()));
    ASSERT_FALSE(fmod.matchesBSON(BSON("a" << BSONNULL)));
}

TEST(InternalSchemaFmodMatchExpression, NegativeRemainders) {
    InternalSchemaFmodMatchExpression fmod;
    ASSERT_OK(fmod.init("a", Decimal128(5), Decimal128(-2.4)));
    ASSERT_FALSE(fmod.matchesBSON(BSON("a" << 7.6)));
    ASSERT_FALSE(fmod.matchesBSON(BSON("a" << 12.4)));
    ASSERT_TRUE(fmod.matchesBSON(BSON("a" << Decimal128(-12.4))));
}

TEST(InternalSchemaFmodMatchExpression, ElemMatchKey) {
    InternalSchemaFmodMatchExpression fmod;
    ASSERT_OK(fmod.init("a", Decimal128(5), Decimal128(2)));
    MatchDetails details;
    details.requestElemMatchKey();
    ASSERT_FALSE(fmod.matchesBSON(BSON("a" << 4), &details));
    ASSERT_FALSE(details.hasElemMatchKey());
    ASSERT_TRUE(fmod.matchesBSON(BSON("a" << 2), &details));
    ASSERT_FALSE(details.hasElemMatchKey());
    ASSERT_TRUE(fmod.matchesBSON(BSON("a" << BSON_ARRAY(1 << 2 << 5)), &details));
    ASSERT_TRUE(details.hasElemMatchKey());
    ASSERT_EQUALS("1", details.elemMatchKey());
}

TEST(InternalSchemaFmodMatchExpression, Equality) {
    InternalSchemaFmodMatchExpression m1;
    InternalSchemaFmodMatchExpression m2;
    InternalSchemaFmodMatchExpression m3;
    InternalSchemaFmodMatchExpression m4;

    ASSERT_OK(m1.init("a", Decimal128(1.7), Decimal128(2)));
    ASSERT_OK(m2.init("a", Decimal128(2), Decimal128(2)));
    ASSERT_OK(m3.init("a", Decimal128(1.7), Decimal128(1)));
    ASSERT_OK(m4.init("b", Decimal128(1.7), Decimal128(2)));

    ASSERT_TRUE(m1.equivalent(&m1));
    ASSERT_FALSE(m1.equivalent(&m2));
    ASSERT_FALSE(m1.equivalent(&m3));
    ASSERT_FALSE(m1.equivalent(&m4));
}
}  // namespace
}  // namespace mongo
