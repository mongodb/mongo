/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(PathAcceptingKeyword, CanParseKnownMatchTypes) {
    ASSERT_TRUE(PathAcceptingKeyword::LESS_THAN ==
                MatchExpressionParser::parsePathAcceptingKeyword(BSON("$lt" << 1).firstElement()));
    ASSERT_TRUE(PathAcceptingKeyword::LESS_THAN_OR_EQUAL ==
                MatchExpressionParser::parsePathAcceptingKeyword(BSON("$lte" << 1).firstElement()));
    ASSERT_TRUE(PathAcceptingKeyword::GREATER_THAN_OR_EQUAL ==
                MatchExpressionParser::parsePathAcceptingKeyword(BSON("$gte" << 1).firstElement()));
    ASSERT_TRUE(PathAcceptingKeyword::GREATER_THAN ==
                MatchExpressionParser::parsePathAcceptingKeyword(BSON("$gt" << 1).firstElement()));
    ASSERT_TRUE(PathAcceptingKeyword::IN_EXPR ==
                MatchExpressionParser::parsePathAcceptingKeyword(BSON("$in" << 1).firstElement()));
    ASSERT_TRUE(PathAcceptingKeyword::NOT_EQUAL ==
                MatchExpressionParser::parsePathAcceptingKeyword(BSON("$ne" << 1).firstElement()));
    ASSERT_TRUE(PathAcceptingKeyword::SIZE == MatchExpressionParser::parsePathAcceptingKeyword(
                                                  BSON("$size" << 1).firstElement()));
    ASSERT_TRUE(PathAcceptingKeyword::ALL ==
                MatchExpressionParser::parsePathAcceptingKeyword(BSON("$all" << 1).firstElement()));
    ASSERT_TRUE(PathAcceptingKeyword::NOT_IN ==
                MatchExpressionParser::parsePathAcceptingKeyword(BSON("$nin" << 1).firstElement()));
    ASSERT_TRUE(PathAcceptingKeyword::EXISTS == MatchExpressionParser::parsePathAcceptingKeyword(
                                                    BSON("$exists" << 1).firstElement()));
    ASSERT_TRUE(PathAcceptingKeyword::MOD ==
                MatchExpressionParser::parsePathAcceptingKeyword(BSON("$mod" << 1).firstElement()));
    ASSERT_TRUE(PathAcceptingKeyword::TYPE == MatchExpressionParser::parsePathAcceptingKeyword(
                                                  BSON("$type" << 1).firstElement()));
    ASSERT_TRUE(PathAcceptingKeyword::REGEX == MatchExpressionParser::parsePathAcceptingKeyword(
                                                   BSON("$regex" << 1).firstElement()));
    ASSERT_TRUE(PathAcceptingKeyword::OPTIONS == MatchExpressionParser::parsePathAcceptingKeyword(
                                                     BSON("$options" << 1).firstElement()));
    ASSERT_TRUE(
        PathAcceptingKeyword::ELEM_MATCH ==
        MatchExpressionParser::parsePathAcceptingKeyword(BSON("$elemMatch" << 1).firstElement()));
    ASSERT_TRUE(PathAcceptingKeyword::GEO_NEAR == MatchExpressionParser::parsePathAcceptingKeyword(
                                                      BSON("$near" << 1).firstElement()));
    ASSERT_TRUE(PathAcceptingKeyword::GEO_NEAR == MatchExpressionParser::parsePathAcceptingKeyword(
                                                      BSON("$geoNear" << 1).firstElement()));
    ASSERT_TRUE(PathAcceptingKeyword::WITHIN == MatchExpressionParser::parsePathAcceptingKeyword(
                                                    BSON("$within" << 1).firstElement()));
    ASSERT_TRUE(PathAcceptingKeyword::WITHIN == MatchExpressionParser::parsePathAcceptingKeyword(
                                                    BSON("$geoWithin" << 1).firstElement()));
    ASSERT_TRUE(PathAcceptingKeyword::GEO_INTERSECTS ==
                MatchExpressionParser::parsePathAcceptingKeyword(
                    BSON("$geoIntersects" << 1).firstElement()));
    ASSERT_TRUE(
        PathAcceptingKeyword::BITS_ALL_SET ==
        MatchExpressionParser::parsePathAcceptingKeyword(BSON("$bitsAllSet" << 1).firstElement()));
    ASSERT_TRUE(PathAcceptingKeyword::BITS_ALL_CLEAR ==
                MatchExpressionParser::parsePathAcceptingKeyword(
                    BSON("$bitsAllClear" << 1).firstElement()));
    ASSERT_TRUE(
        PathAcceptingKeyword::BITS_ANY_SET ==
        MatchExpressionParser::parsePathAcceptingKeyword(BSON("$bitsAnySet" << 1).firstElement()));
    ASSERT_TRUE(PathAcceptingKeyword::BITS_ANY_CLEAR ==
                MatchExpressionParser::parsePathAcceptingKeyword(
                    BSON("$bitsAnyClear" << 1).firstElement()));
    ASSERT_TRUE(PathAcceptingKeyword::INTERNAL_SCHEMA_MIN_ITEMS ==
                MatchExpressionParser::parsePathAcceptingKeyword(
                    BSON("$_internalSchemaMinItems" << 1).firstElement()));
    ASSERT_TRUE(PathAcceptingKeyword::INTERNAL_SCHEMA_MAX_ITEMS ==
                MatchExpressionParser::parsePathAcceptingKeyword(
                    BSON("$_internalSchemaMaxItems" << 1).firstElement()));
    ASSERT_TRUE(PathAcceptingKeyword::INTERNAL_SCHEMA_UNIQUE_ITEMS ==
                MatchExpressionParser::parsePathAcceptingKeyword(
                    BSON("$_internalSchemaUniqueItems" << 1).firstElement()));
    ASSERT_TRUE(PathAcceptingKeyword::INTERNAL_SCHEMA_OBJECT_MATCH ==
                MatchExpressionParser::parsePathAcceptingKeyword(
                    BSON("$_internalSchemaObjectMatch" << 1).firstElement()));
    ASSERT_TRUE(PathAcceptingKeyword::INTERNAL_SCHEMA_MIN_LENGTH ==
                MatchExpressionParser::parsePathAcceptingKeyword(
                    BSON("$_internalSchemaMinLength" << 1).firstElement()));
    ASSERT_TRUE(PathAcceptingKeyword::INTERNAL_SCHEMA_MAX_LENGTH ==
                MatchExpressionParser::parsePathAcceptingKeyword(
                    BSON("$_internalSchemaMaxLength" << 1).firstElement()));
}

TEST(PathAcceptingKeyword, EqualityMatchReturnsDefault) {
    // 'boost::none' is the default when none specified.
    ASSERT_TRUE(boost::none ==
                MatchExpressionParser::parsePathAcceptingKeyword(BSON("$eq" << 1).firstElement()));
    // Should return default specified by caller.
    ASSERT_TRUE(PathAcceptingKeyword::GEO_NEAR ==
                MatchExpressionParser::parsePathAcceptingKeyword(BSON("$eq" << 1).firstElement(),
                                                                 PathAcceptingKeyword::GEO_NEAR));
}

TEST(PathAcceptingKeyword, UnknownExpressionReturnsDefault) {
    // Non-existent expression starting with '$'.
    ASSERT_TRUE(PathAcceptingKeyword::GEO_INTERSECTS ==
                MatchExpressionParser::parsePathAcceptingKeyword(
                    BSON("$foo" << 1).firstElement(), PathAcceptingKeyword::GEO_INTERSECTS));
    // Existing expression but missing leading '$'.
    ASSERT_TRUE(PathAcceptingKeyword::NOT_IN ==
                MatchExpressionParser::parsePathAcceptingKeyword(BSON("size" << 1).firstElement(),
                                                                 PathAcceptingKeyword::NOT_IN));
}

TEST(PathAcceptingKeyword, EmptyBSONElemReturnsDefault) {
    BSONElement emptyElem;
    ASSERT_TRUE(boost::none == MatchExpressionParser::parsePathAcceptingKeyword(emptyElem));
}

}  // namespace
}  // namespace mongo
