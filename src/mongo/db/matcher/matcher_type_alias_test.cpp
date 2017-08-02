/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/matcher/matcher_type_alias.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(MatcherTypeAliasTest, ParseFromStringCanParseNumberAlias) {
    auto result = MatcherTypeAlias::parseFromStringAlias("number");
    ASSERT_OK(result.getStatus());
    ASSERT_TRUE(result.getValue().allNumbers);
}

TEST(MatcherTypeAliasTest, ParseFromStringCanParseLongAlias) {
    auto result = MatcherTypeAlias::parseFromStringAlias("long");
    ASSERT_OK(result.getStatus());
    ASSERT_FALSE(result.getValue().allNumbers);
    ASSERT_EQ(result.getValue().bsonType, BSONType::NumberLong);
}

TEST(MatcherTypeAliasTest, ParseFromStringFailsToParseUnknownAlias) {
    auto result = MatcherTypeAlias::parseFromStringAlias("unknown");
    ASSERT_NOT_OK(result.getStatus());
}

TEST(MatcherTypeAliasTest, ParseFromElementCanParseNumberAlias) {
    auto obj = BSON(""
                    << "number");
    auto result = MatcherTypeAlias::parse(obj.firstElement());
    ASSERT_OK(result.getStatus());
    ASSERT_TRUE(result.getValue().allNumbers);
}

TEST(MatcherTypeAliasTest, ParseFromElementCanParseLongAlias) {
    auto obj = BSON(""
                    << "long");
    auto result = MatcherTypeAlias::parse(obj.firstElement());
    ASSERT_OK(result.getStatus());
    ASSERT_FALSE(result.getValue().allNumbers);
    ASSERT_EQ(result.getValue().bsonType, BSONType::NumberLong);
}

TEST(MatcherTypeAliasTest, ParseFromElementFailsToParseUnknownAlias) {
    auto obj = BSON(""
                    << "unknown");
    auto result = MatcherTypeAlias::parse(obj.firstElement());
    ASSERT_NOT_OK(result.getStatus());
}

TEST(MatcherTypeAliasTest, ParseFromElementFailsToParseWrongElementType) {
    auto obj = BSON("" << BSON(""
                               << ""));
    auto result = MatcherTypeAlias::parse(obj.firstElement());
    ASSERT_NOT_OK(result.getStatus());

    obj = fromjson("{'': null}");
    result = MatcherTypeAlias::parse(obj.firstElement());
    ASSERT_NOT_OK(result.getStatus());
}

TEST(MatcherTypeAliasTest, ParseFromElementFailsToParseUnknownBSONType) {
    auto obj = BSON("" << 99);
    auto result = MatcherTypeAlias::parse(obj.firstElement());
    ASSERT_NOT_OK(result.getStatus());
}

TEST(MatcherTypeAliasTest, ParseFromElementCanParseIntegerTypeCode) {
    auto obj = BSON("" << 2);
    auto result = MatcherTypeAlias::parse(obj.firstElement());
    ASSERT_OK(result.getStatus());
    ASSERT_FALSE(result.getValue().allNumbers);
    ASSERT_EQ(result.getValue().bsonType, BSONType::String);
}

TEST(MatcherTypeAliasTest, ParseFromElementCanParseDoubleTypeCode) {
    auto obj = BSON("" << 2.0);
    auto result = MatcherTypeAlias::parse(obj.firstElement());
    ASSERT_OK(result.getStatus());
    ASSERT_FALSE(result.getValue().allNumbers);
    ASSERT_EQ(result.getValue().bsonType, BSONType::String);
}

}  // namespace
}  // namespace mongo
