// expression_parser_leaf_test.cpp

/**
 *    Copyright (C) 2013 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/unittest/unittest.h"

#include "mongo/db/matcher/expression_parser.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/extensions_callback_disallow_extensions.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/log.h"

namespace mongo {

using std::endl;
using std::string;

TEST(MatchExpressionParserLeafTest, NullCollation) {
    BSONObj query = BSON("x"
                         << "string");
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_TRUE(result.isOK());
    ASSERT_EQUALS(MatchExpression::EQ, result.getValue()->matchType());
    EqualityMatchExpression* match = static_cast<EqualityMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == collator);
}


TEST(MatchExpressionParserLeafTest, Collation) {
    BSONObj query = BSON("x"
                         << "string");
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), &collator);
    ASSERT_TRUE(result.isOK());
    ASSERT_EQUALS(MatchExpression::EQ, result.getValue()->matchType());
    EqualityMatchExpression* match = static_cast<EqualityMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == &collator);
}

TEST(MatchExpressionParserLeafTest, SimpleEQ2) {
    BSONObj query = BSON("x" << BSON("$eq" << 2));
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_TRUE(result.isOK());

    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 1)));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << 2)));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 3)));
}

TEST(MatchExpressionParserLeafTest, SimpleEQUndefined) {
    BSONObj query = BSON("x" << BSON("$eq" << BSONUndefined));
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_FALSE(result.isOK());
}

TEST(MatchExpressionParserLeafTest, EQNullCollation) {
    BSONObj query = BSON("x" << BSON("$eq"
                                     << "string"));
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_TRUE(result.isOK());
    ASSERT_EQUALS(MatchExpression::EQ, result.getValue()->matchType());
    EqualityMatchExpression* match = static_cast<EqualityMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == collator);
}


TEST(MatchExpressionParserLeafTest, EQCollation) {
    BSONObj query = BSON("x" << BSON("$eq"
                                     << "string"));
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), &collator);
    ASSERT_TRUE(result.isOK());
    ASSERT_EQUALS(MatchExpression::EQ, result.getValue()->matchType());
    EqualityMatchExpression* match = static_cast<EqualityMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == &collator);
}

TEST(MatchExpressionParserLeafTest, SimpleGT1) {
    BSONObj query = BSON("x" << BSON("$gt" << 2));
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_TRUE(result.isOK());

    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 2)));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << 3)));
}

TEST(MatchExpressionParserLeafTest, GTNullCollation) {
    BSONObj query = BSON("x" << BSON("$gt"
                                     << "abc"));
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_TRUE(result.isOK());
    ASSERT_EQUALS(MatchExpression::GT, result.getValue()->matchType());
    GTMatchExpression* match = static_cast<GTMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == collator);
}


TEST(MatchExpressionParserLeafTest, GTCollation) {
    BSONObj query = BSON("x" << BSON("$gt"
                                     << "abc"));
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), &collator);
    ASSERT_TRUE(result.isOK());
    ASSERT_EQUALS(MatchExpression::GT, result.getValue()->matchType());
    GTMatchExpression* match = static_cast<GTMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == &collator);
}

TEST(MatchExpressionParserLeafTest, SimpleLT1) {
    BSONObj query = BSON("x" << BSON("$lt" << 2));
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_TRUE(result.isOK());

    ASSERT(result.getValue()->matchesBSON(BSON("x" << 1)));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 2)));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 3)));
}

TEST(MatchExpressionParserLeafTest, LTNullCollation) {
    BSONObj query = BSON("x" << BSON("$lt"
                                     << "abc"));
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_TRUE(result.isOK());
    ASSERT_EQUALS(MatchExpression::LT, result.getValue()->matchType());
    LTMatchExpression* match = static_cast<LTMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == collator);
}


TEST(MatchExpressionParserLeafTest, LTCollation) {
    BSONObj query = BSON("x" << BSON("$lt"
                                     << "abc"));
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), &collator);
    ASSERT_TRUE(result.isOK());
    ASSERT_EQUALS(MatchExpression::LT, result.getValue()->matchType());
    LTMatchExpression* match = static_cast<LTMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == &collator);
}

TEST(MatchExpressionParserLeafTest, SimpleGTE1) {
    BSONObj query = BSON("x" << BSON("$gte" << 2));
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_TRUE(result.isOK());

    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 1)));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << 2)));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << 3)));
}

TEST(MatchExpressionParserLeafTest, GTENullCollation) {
    BSONObj query = BSON("x" << BSON("$gte"
                                     << "abc"));
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_TRUE(result.isOK());
    ASSERT_EQUALS(MatchExpression::GTE, result.getValue()->matchType());
    GTEMatchExpression* match = static_cast<GTEMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == collator);
}


TEST(MatchExpressionParserLeafTest, GTECollation) {
    BSONObj query = BSON("x" << BSON("$gte"
                                     << "abc"));
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), &collator);
    ASSERT_TRUE(result.isOK());
    ASSERT_EQUALS(MatchExpression::GTE, result.getValue()->matchType());
    GTEMatchExpression* match = static_cast<GTEMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == &collator);
}

TEST(MatchExpressionParserLeafTest, SimpleLTE1) {
    BSONObj query = BSON("x" << BSON("$lte" << 2));
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_TRUE(result.isOK());

    ASSERT(result.getValue()->matchesBSON(BSON("x" << 1)));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << 2)));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 3)));
}

TEST(MatchExpressionParserLeafTest, LTENullCollation) {
    BSONObj query = BSON("x" << BSON("$lte"
                                     << "abc"));
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_TRUE(result.isOK());
    ASSERT_EQUALS(MatchExpression::LTE, result.getValue()->matchType());
    LTEMatchExpression* match = static_cast<LTEMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == collator);
}


TEST(MatchExpressionParserLeafTest, LTECollation) {
    BSONObj query = BSON("x" << BSON("$lte"
                                     << "abc"));
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), &collator);
    ASSERT_TRUE(result.isOK());
    ASSERT_EQUALS(MatchExpression::LTE, result.getValue()->matchType());
    LTEMatchExpression* match = static_cast<LTEMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == &collator);
}

TEST(MatchExpressionParserLeafTest, SimpleNE1) {
    BSONObj query = BSON("x" << BSON("$ne" << 2));
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_TRUE(result.isOK());

    ASSERT(result.getValue()->matchesBSON(BSON("x" << 1)));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 2)));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << 3)));
}

TEST(MatchExpressionParserLeafTest, NENullCollation) {
    BSONObj query = BSON("x" << BSON("$ne"
                                     << "string"));
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_TRUE(result.isOK());
    ASSERT_EQUALS(MatchExpression::NOT, result.getValue()->matchType());
    MatchExpression* child = result.getValue()->getChild(0);
    ASSERT_EQUALS(MatchExpression::EQ, child->matchType());
    EqualityMatchExpression* eqMatch = static_cast<EqualityMatchExpression*>(child);
    ASSERT_TRUE(eqMatch->getCollator() == collator);
}


TEST(MatchExpressionParserLeafTest, NECollation) {
    BSONObj query = BSON("x" << BSON("$ne"
                                     << "string"));
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), &collator);
    ASSERT_TRUE(result.isOK());
    ASSERT_EQUALS(MatchExpression::NOT, result.getValue()->matchType());
    MatchExpression* child = result.getValue()->getChild(0);
    ASSERT_EQUALS(MatchExpression::EQ, child->matchType());
    EqualityMatchExpression* eqMatch = static_cast<EqualityMatchExpression*>(child);
    ASSERT_TRUE(eqMatch->getCollator() == &collator);
}

TEST(MatchExpressionParserLeafTest, SimpleModBad1) {
    BSONObj query = BSON("x" << BSON("$mod" << BSON_ARRAY(3 << 2)));
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_TRUE(result.isOK());

    query = BSON("x" << BSON("$mod" << BSON_ARRAY(3)));
    result = MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_TRUE(!result.isOK());

    query = BSON("x" << BSON("$mod" << BSON_ARRAY(3 << 2 << 4)));
    result = MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_TRUE(!result.isOK());

    query = BSON("x" << BSON("$mod" << BSON_ARRAY("q" << 2)));
    result = MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_TRUE(!result.isOK());

    query = BSON("x" << BSON("$mod" << 3));
    result = MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_TRUE(!result.isOK());

    query = BSON("x" << BSON("$mod" << BSON("a" << 1 << "b" << 2)));
    result = MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_TRUE(!result.isOK());
}

TEST(MatchExpressionParserLeafTest, SimpleMod1) {
    BSONObj query = BSON("x" << BSON("$mod" << BSON_ARRAY(3 << 2)));
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_TRUE(result.isOK());

    ASSERT(result.getValue()->matchesBSON(BSON("x" << 5)));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 4)));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << 8)));
}

TEST(MatchExpressionParserLeafTest, SimpleModNotNumber) {
    BSONObj query = BSON("x" << BSON("$mod" << BSON_ARRAY(2 << "r")));
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_TRUE(result.isOK());

    ASSERT(result.getValue()->matchesBSON(BSON("x" << 2)));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << 4)));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 5)));
    ASSERT(!result.getValue()->matchesBSON(BSON("x"
                                                << "a")));
}

TEST(MatchExpressionParserLeafTest, IdCollation) {
    BSONObj query = BSON("$id"
                         << "string");
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_TRUE(result.isOK());
    ASSERT_EQUALS(MatchExpression::EQ, result.getValue()->matchType());
    EqualityMatchExpression* match = static_cast<EqualityMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == collator);
}

TEST(MatchExpressionParserLeafTest, IdNullCollation) {
    BSONObj query = BSON("$id"
                         << "string");
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), &collator);
    ASSERT_TRUE(result.isOK());
    ASSERT_EQUALS(MatchExpression::EQ, result.getValue()->matchType());
    EqualityMatchExpression* match = static_cast<EqualityMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == &collator);
}

TEST(MatchExpressionParserLeafTest, RefCollation) {
    BSONObj query = BSON("$ref"
                         << "coll");
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), &collator);
    ASSERT_TRUE(result.isOK());
    ASSERT_EQUALS(MatchExpression::EQ, result.getValue()->matchType());
    EqualityMatchExpression* match = static_cast<EqualityMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == nullptr);
}

TEST(MatchExpressionParserLeafTest, DbCollation) {
    BSONObj query = BSON("$db"
                         << "db");
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), &collator);
    ASSERT_TRUE(result.isOK());
    ASSERT_EQUALS(MatchExpression::EQ, result.getValue()->matchType());
    EqualityMatchExpression* match = static_cast<EqualityMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == nullptr);
}

TEST(MatchExpressionParserLeafTest, SimpleIN1) {
    BSONObj query = BSON("x" << BSON("$in" << BSON_ARRAY(2 << 3)));
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_TRUE(result.isOK());

    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 1)));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << 2)));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << 3)));
}

TEST(MatchExpressionParserLeafTest, INNullCollation) {
    BSONObj query = BSON("x" << BSON("$in" << BSON_ARRAY("string")));
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_TRUE(result.isOK());
    ASSERT_EQUALS(MatchExpression::MATCH_IN, result.getValue()->matchType());
    InMatchExpression* match = static_cast<InMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == collator);
}

TEST(MatchExpressionParserLeafTest, INCollation) {
    BSONObj query = BSON("x" << BSON("$in" << BSON_ARRAY("string")));
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), &collator);
    ASSERT_TRUE(result.isOK());
    ASSERT_EQUALS(MatchExpression::MATCH_IN, result.getValue()->matchType());
    InMatchExpression* match = static_cast<InMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == &collator);
}

TEST(MatchExpressionParserLeafTest, INSingleDBRef) {
    OID oid = OID::gen();
    BSONObj query = BSON("x" << BSON("$in" << BSON_ARRAY(BSON("$ref"
                                                              << "coll"
                                                              << "$id"
                                                              << oid
                                                              << "$db"
                                                              << "db"))));
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_TRUE(result.isOK());

    OID oidx = OID::gen();
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON("$ref"
                                                            << "collx"
                                                            << "$id"
                                                            << oidx
                                                            << "$db"
                                                            << "db"))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON("$ref"
                                                            << "coll"
                                                            << "$id"
                                                            << oidx
                                                            << "$db"
                                                            << "db"))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON("$id" << oid << "$ref"
                                                                  << "coll"
                                                                  << "$db"
                                                                  << "db"))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON("$id" << oid << "$ref"
                                                                  << "coll"
                                                                  << "$db"
                                                                  << "db"))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(BSON("$id" << oid << "$ref"
                                                                             << "coll"
                                                                             << "$db"
                                                                             << "db")))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON("$ref"
                                                            << "coll"
                                                            << "$id"
                                                            << oid
                                                            << "$db"
                                                            << "dbx"))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON("$db"
                                                            << "db"
                                                            << "$ref"
                                                            << "coll"
                                                            << "$id"
                                                            << oid))));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON("$ref"
                                                           << "coll"
                                                           << "$id"
                                                           << oid
                                                           << "$db"
                                                           << "db"))));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(BSON("$ref"
                                                                      << "coll"
                                                                      << "$id"
                                                                      << oid
                                                                      << "$db"
                                                                      << "db")))));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(BSON("$ref"
                                                                      << "collx"
                                                                      << "$id"
                                                                      << oidx
                                                                      << "$db"
                                                                      << "db")
                                                                 << BSON("$ref"
                                                                         << "coll"
                                                                         << "$id"
                                                                         << oid
                                                                         << "$db"
                                                                         << "db")))));
}

TEST(MatchExpressionParserLeafTest, INMultipleDBRef) {
    OID oid = OID::gen();
    OID oidy = OID::gen();
    BSONObj query = BSON("x" << BSON("$in" << BSON_ARRAY(BSON("$ref"
                                                              << "colly"
                                                              << "$id"
                                                              << oidy
                                                              << "$db"
                                                              << "db")
                                                         << BSON("$ref"
                                                                 << "coll"
                                                                 << "$id"
                                                                 << oid
                                                                 << "$db"
                                                                 << "db"))));
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_TRUE(result.isOK());

    OID oidx = OID::gen();
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON("$ref"
                                                            << "collx"
                                                            << "$id"
                                                            << oidx
                                                            << "$db"
                                                            << "db"))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON("$ref"
                                                            << "coll"
                                                            << "$id"
                                                            << oidx
                                                            << "$db"
                                                            << "db"))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON("$id" << oid << "$ref"
                                                                  << "coll"
                                                                  << "$db"
                                                                  << "db"))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(BSON("$ref"
                                                                       << "coll"
                                                                       << "$id"
                                                                       << oidy
                                                                       << "$db"
                                                                       << "db")))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(BSON("$ref"
                                                                       << "colly"
                                                                       << "$id"
                                                                       << oid
                                                                       << "$db"
                                                                       << "db")))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(BSON("$id" << oid << "$ref"
                                                                             << "coll"
                                                                             << "$db"
                                                                             << "db")))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(BSON("$ref"
                                                                       << "coll"
                                                                       << "$id"
                                                                       << oid
                                                                       << "$db"
                                                                       << "dbx")))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(BSON("$id" << oidy << "$ref"
                                                                             << "colly"
                                                                             << "$db"
                                                                             << "db")))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(BSON("$ref"
                                                                       << "collx"
                                                                       << "$id"
                                                                       << oidx
                                                                       << "$db"
                                                                       << "db")
                                                                  << BSON("$ref"
                                                                          << "coll"
                                                                          << "$id"
                                                                          << oidx
                                                                          << "$db"
                                                                          << "db")))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(BSON("$ref"
                                                                       << "collx"
                                                                       << "$id"
                                                                       << oidx
                                                                       << "$db"
                                                                       << "db")
                                                                  << BSON("$ref"
                                                                          << "colly"
                                                                          << "$id"
                                                                          << oidx
                                                                          << "$db"
                                                                          << "db")))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(BSON("$ref"
                                                                       << "collx"
                                                                       << "$id"
                                                                       << oidx
                                                                       << "$db"
                                                                       << "db")
                                                                  << BSON("$ref"
                                                                          << "coll"
                                                                          << "$id"
                                                                          << oid
                                                                          << "$db"
                                                                          << "dbx")))));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON("$ref"
                                                           << "coll"
                                                           << "$id"
                                                           << oid
                                                           << "$db"
                                                           << "db"))));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON("$ref"
                                                           << "colly"
                                                           << "$id"
                                                           << oidy
                                                           << "$db"
                                                           << "db"))));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(BSON("$ref"
                                                                      << "coll"
                                                                      << "$id"
                                                                      << oid
                                                                      << "$db"
                                                                      << "db")))));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(BSON("$ref"
                                                                      << "colly"
                                                                      << "$id"
                                                                      << oidy
                                                                      << "$db"
                                                                      << "db")))));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(BSON("$ref"
                                                                      << "collx"
                                                                      << "$id"
                                                                      << oidx
                                                                      << "$db"
                                                                      << "db")
                                                                 << BSON("$ref"
                                                                         << "coll"
                                                                         << "$id"
                                                                         << oid
                                                                         << "$db"
                                                                         << "db")))));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(BSON("$ref"
                                                                      << "collx"
                                                                      << "$id"
                                                                      << oidx
                                                                      << "$db"
                                                                      << "db")
                                                                 << BSON("$ref"
                                                                         << "colly"
                                                                         << "$id"
                                                                         << oidy
                                                                         << "$db"
                                                                         << "db")))));
}

TEST(MatchExpressionParserLeafTest, INDBRefWithOptionalField1) {
    OID oid = OID::gen();
    BSONObj query = BSON("x" << BSON("$in" << BSON_ARRAY(BSON("$ref"
                                                              << "coll"
                                                              << "$id"
                                                              << oid
                                                              << "foo"
                                                              << 12345))));
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_TRUE(result.isOK());

    OID oidx = OID::gen();
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON("$ref"
                                                            << "coll"
                                                            << "$id"
                                                            << oidx
                                                            << "$db"
                                                            << "db"))));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(BSON("$ref"
                                                                      << "coll"
                                                                      << "$id"
                                                                      << oid
                                                                      << "foo"
                                                                      << 12345)))));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(BSON("$ref"
                                                                      << "collx"
                                                                      << "$id"
                                                                      << oidx
                                                                      << "foo"
                                                                      << 12345)
                                                                 << BSON("$ref"
                                                                         << "coll"
                                                                         << "$id"
                                                                         << oid
                                                                         << "foo"
                                                                         << 12345)))));
}

TEST(MatchExpressionParserLeafTest, INInvalidDBRefs) {
    // missing $id
    BSONObj query = BSON("x" << BSON("$in" << BSON_ARRAY(BSON("$ref"
                                                              << "coll"))));
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    result = MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);

    // second field is not $id
    query = BSON("x" << BSON("$in" << BSON_ARRAY(BSON("$ref"
                                                      << "coll"
                                                      << "$foo"
                                                      << 1))));
    result = MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_FALSE(result.isOK());

    OID oid = OID::gen();

    // missing $ref field
    query = BSON("x" << BSON("$in" << BSON_ARRAY(BSON("$id" << oid << "foo" << 3))));
    result = MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_FALSE(result.isOK());

    // missing $id and $ref field
    query = BSON("x" << BSON("$in" << BSON_ARRAY(BSON("$db"
                                                      << "test"
                                                      << "foo"
                                                      << 3))));
    result = MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_FALSE(result.isOK());
}

TEST(MatchExpressionParserLeafTest, INExpressionDocument) {
    BSONObj query = BSON("x" << BSON("$in" << BSON_ARRAY(BSON("$foo" << 1))));
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_FALSE(result.isOK());
}

TEST(MatchExpressionParserLeafTest, INNotArray) {
    BSONObj query = BSON("x" << BSON("$in" << 5));
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_FALSE(result.isOK());
}

TEST(MatchExpressionParserLeafTest, INUndefined) {
    BSONObj query = BSON("x" << BSON("$in" << BSON_ARRAY(BSONUndefined)));
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_FALSE(result.isOK());
}

TEST(MatchExpressionParserLeafTest, INNotElemMatch) {
    BSONObj query = BSON("x" << BSON("$in" << BSON_ARRAY(BSON("$elemMatch" << 1))));
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_FALSE(result.isOK());
}

TEST(MatchExpressionParserLeafTest, INRegexTooLong) {
    string tooLargePattern(50 * 1000, 'z');
    BSONObjBuilder inArray;
    inArray.appendRegex("0", tooLargePattern, "");
    BSONObjBuilder operand;
    operand.appendArray("$in", inArray.obj());
    BSONObj query = BSON("x" << operand.obj());
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_FALSE(result.isOK());
}

TEST(MatchExpressionParserLeafTest, INRegexTooLong2) {
    string tooLargePattern(50 * 1000, 'z');
    BSONObj query = BSON("x" << BSON("$in" << BSON_ARRAY(BSON("$regex" << tooLargePattern))));
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_FALSE(result.isOK());
}

TEST(MatchExpressionParserLeafTest, INRegexStuff) {
    BSONObjBuilder inArray;
    inArray.appendRegex("0", "^a", "");
    inArray.appendRegex("1", "B", "i");
    inArray.append("2", 4);
    BSONObjBuilder operand;
    operand.appendArray("$in", inArray.obj());

    BSONObj query = BSON("a" << operand.obj());
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_TRUE(result.isOK());

    BSONObj matchFirst = BSON("a"
                              << "ax");
    BSONObj matchFirstRegex = BSONObjBuilder().appendRegex("a", "^a", "").obj();
    BSONObj matchSecond = BSON("a"
                               << "qqb");
    BSONObj matchSecondRegex = BSONObjBuilder().appendRegex("a", "B", "i").obj();
    BSONObj matchThird = BSON("a" << 4);
    BSONObj notMatch = BSON("a"
                            << "l");
    BSONObj notMatchRegex = BSONObjBuilder().appendRegex("a", "B", "").obj();

    ASSERT(result.getValue()->matchesBSON(matchFirst));
    ASSERT(result.getValue()->matchesBSON(matchFirstRegex));
    ASSERT(result.getValue()->matchesBSON(matchSecond));
    ASSERT(result.getValue()->matchesBSON(matchSecondRegex));
    ASSERT(result.getValue()->matchesBSON(matchThird));
    ASSERT(!result.getValue()->matchesBSON(notMatch));
    ASSERT(!result.getValue()->matchesBSON(notMatchRegex));
}

TEST(MatchExpressionParserLeafTest, SimpleNIN1) {
    BSONObj query = BSON("x" << BSON("$nin" << BSON_ARRAY(2 << 3)));
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_TRUE(result.isOK());

    ASSERT(result.getValue()->matchesBSON(BSON("x" << 1)));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 2)));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 3)));
}

TEST(MatchExpressionParserLeafTest, NINNotArray) {
    BSONObj query = BSON("x" << BSON("$nin" << 5));
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_FALSE(result.isOK());
}

TEST(MatchExpressionParserLeafTest, NINNullCollation) {
    BSONObj query = BSON("x" << BSON("$nin" << BSON_ARRAY("string")));
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_TRUE(result.isOK());
    ASSERT_EQUALS(MatchExpression::NOT, result.getValue()->matchType());
    MatchExpression* child = result.getValue()->getChild(0);
    ASSERT_EQUALS(MatchExpression::MATCH_IN, child->matchType());
    InMatchExpression* inMatch = static_cast<InMatchExpression*>(child);
    ASSERT_TRUE(inMatch->getCollator() == collator);
}

TEST(MatchExpressionParserLeafTest, NINCollation) {
    BSONObj query = BSON("x" << BSON("$nin" << BSON_ARRAY("string")));
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), &collator);
    ASSERT_TRUE(result.isOK());
    ASSERT_EQUALS(MatchExpression::NOT, result.getValue()->matchType());
    MatchExpression* child = result.getValue()->getChild(0);
    ASSERT_EQUALS(MatchExpression::MATCH_IN, child->matchType());
    InMatchExpression* inMatch = static_cast<InMatchExpression*>(child);
    ASSERT_TRUE(inMatch->getCollator() == &collator);
}

TEST(MatchExpressionParserLeafTest, Regex1) {
    BSONObjBuilder b;
    b.appendRegex("x", "abc", "i");
    BSONObj query = b.obj();
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_TRUE(result.isOK());

    ASSERT(result.getValue()->matchesBSON(BSON("x"
                                               << "abc")));
    ASSERT(result.getValue()->matchesBSON(BSON("x"
                                               << "ABC")));
    ASSERT(!result.getValue()->matchesBSON(BSON("x"
                                                << "AC")));
}

TEST(MatchExpressionParserLeafTest, Regex2) {
    BSONObj query = BSON("x" << BSON("$regex"
                                     << "abc"
                                     << "$options"
                                     << "i"));
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_TRUE(result.isOK());

    ASSERT(result.getValue()->matchesBSON(BSON("x"
                                               << "abc")));
    ASSERT(result.getValue()->matchesBSON(BSON("x"
                                               << "ABC")));
    ASSERT(!result.getValue()->matchesBSON(BSON("x"
                                                << "AC")));
}

TEST(MatchExpressionParserLeafTest, Regex3) {
    BSONObj query = BSON("x" << BSON("$options"
                                     << "i"
                                     << "$regex"
                                     << "abc"));
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    log() << "result: " << result.getStatus() << endl;
    ASSERT_TRUE(result.isOK());

    ASSERT(result.getValue()->matchesBSON(BSON("x"
                                               << "abc")));
    ASSERT(result.getValue()->matchesBSON(BSON("x"
                                               << "ABC")));
    ASSERT(!result.getValue()->matchesBSON(BSON("x"
                                                << "AC")));
}


TEST(MatchExpressionParserLeafTest, RegexBad) {
    BSONObj query = BSON("x" << BSON("$regex"
                                     << "abc"
                                     << "$optionas"
                                     << "i"));
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_FALSE(result.isOK());

    // $regex does not with numbers
    query = BSON("x" << BSON("$regex" << 123));
    result = MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_FALSE(result.isOK());

    query = BSON("x" << BSON("$regex" << BSON_ARRAY("abc")));
    result = MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_FALSE(result.isOK());

    query = BSON("x" << BSON("$optionas"
                             << "i"));
    result = MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_FALSE(result.isOK());

    query = BSON("x" << BSON("$options"
                             << "i"));
    result = MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_FALSE(result.isOK());
}

TEST(MatchExpressionParserLeafTest, RegexEmbeddedNULByte) {
    BSONObj query = BSON("x" << BSON("$regex"
                                     << "^a\\x00b"));
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_TRUE(result.isOK());

    const auto value = "a\0b"_sd;
    ASSERT(result.getValue()->matchesBSON(BSON("x" << value)));
    ASSERT(!result.getValue()->matchesBSON(BSON("x"
                                                << "a")));
}

TEST(MatchExpressionParserLeafTest, ExistsYes1) {
    BSONObjBuilder b;
    b.appendBool("$exists", true);
    BSONObj query = BSON("x" << b.obj());
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_TRUE(result.isOK());

    ASSERT(result.getValue()->matchesBSON(BSON("x"
                                               << "abc")));
    ASSERT(!result.getValue()->matchesBSON(BSON("y"
                                                << "AC")));
}

TEST(MatchExpressionParserLeafTest, ExistsNO1) {
    BSONObjBuilder b;
    b.appendBool("$exists", false);
    BSONObj query = BSON("x" << b.obj());
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_TRUE(result.isOK());

    ASSERT(!result.getValue()->matchesBSON(BSON("x"
                                                << "abc")));
    ASSERT(result.getValue()->matchesBSON(BSON("y"
                                               << "AC")));
}

TEST(MatchExpressionParserLeafTest, Type1) {
    BSONObj query = BSON("x" << BSON("$type" << String));
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_TRUE(result.isOK());

    ASSERT(result.getValue()->matchesBSON(BSON("x"
                                               << "abc")));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 5)));
}

TEST(MatchExpressionParserLeafTest, Type2) {
    BSONObj query = BSON("x" << BSON("$type" << (double)NumberDouble));
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_TRUE(result.isOK());

    ASSERT(result.getValue()->matchesBSON(BSON("x" << 5.3)));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 5)));
}

TEST(MatchExpressionParserLeafTest, TypeDoubleOperator) {
    BSONObj query = BSON("x" << BSON("$type" << 1.5));
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_TRUE(result.isOK());

    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 5.3)));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 5)));
}

TEST(MatchExpressionParserLeafTest, TypeDecimalOperator) {
    BSONObj query = BSON("x" << BSON("$type" << mongo::NumberDecimal));
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_TRUE(result.isOK());

    ASSERT_FALSE(result.getValue()->matchesBSON(BSON("x" << 5.3)));
    ASSERT_TRUE(result.getValue()->matchesBSON(BSON("x" << mongo::Decimal128("1"))));
}

TEST(MatchExpressionParserLeafTest, TypeNull) {
    BSONObj query = BSON("x" << BSON("$type" << jstNULL));
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_TRUE(result.isOK());

    ASSERT(!result.getValue()->matchesBSON(BSONObj()));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 5)));
    BSONObjBuilder b;
    b.appendNull("x");
    ASSERT(result.getValue()->matchesBSON(b.obj()));
}

TEST(MatchExpressionParserLeafTest, TypeBadType) {
    BSONObjBuilder b;
    b.append("$type", (JSTypeMax + 1));
    BSONObj query = BSON("x" << b.obj());
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(MatchExpressionParserLeafTest, TypeBad) {
    BSONObj query = BSON("x" << BSON("$type" << BSON("x" << 1)));
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression result =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_FALSE(result.isOK());
}

TEST(MatchExpressionParserLeafTest, TypeBadString) {
    const CollatorInterface* collator = nullptr;
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$type: null}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$type: true}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$type: {}}}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$type: ObjectId('000000000000000000000000')}}"),
                                     ExtensionsCallbackDisallowExtensions(),
                                     collator)
            .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$type: []}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
}

TEST(MatchExpressionParserLeafTest, TypeStringnameDouble) {
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression typeNumberDouble = MatchExpressionParser::parse(
        fromjson("{a: {$type: 'double'}}"), ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_OK(typeNumberDouble.getStatus());
    TypeMatchExpression* tmeNumberDouble =
        static_cast<TypeMatchExpression*>(typeNumberDouble.getValue().get());
    ASSERT(tmeNumberDouble->getType() == NumberDouble);
    ASSERT_TRUE(tmeNumberDouble->matchesBSON(fromjson("{a: 5.4}")));
    ASSERT_FALSE(tmeNumberDouble->matchesBSON(fromjson("{a: NumberInt(5)}")));
}

TEST(MatchExpressionParserLeafTest, TypeStringNameNumberDecimal) {
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression typeNumberDecimal = MatchExpressionParser::parse(
        fromjson("{a: {$type: 'decimal'}}"), ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_OK(typeNumberDecimal.getStatus());
    TypeMatchExpression* tmeNumberDecimal =
        static_cast<TypeMatchExpression*>(typeNumberDecimal.getValue().get());
    ASSERT(tmeNumberDecimal->getType() == NumberDecimal);
    ASSERT_TRUE(tmeNumberDecimal->matchesBSON(BSON("a" << mongo::Decimal128("1"))));
    ASSERT_FALSE(tmeNumberDecimal->matchesBSON(fromjson("{a: true}")));
}

TEST(MatchExpressionParserLeafTest, TypeStringnameNumberInt) {
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression typeNumberInt = MatchExpressionParser::parse(
        fromjson("{a: {$type: 'int'}}"), ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_OK(typeNumberInt.getStatus());
    TypeMatchExpression* tmeNumberInt =
        static_cast<TypeMatchExpression*>(typeNumberInt.getValue().get());
    ASSERT(tmeNumberInt->getType() == NumberInt);
    ASSERT_TRUE(tmeNumberInt->matchesBSON(fromjson("{a: NumberInt(5)}")));
    ASSERT_FALSE(tmeNumberInt->matchesBSON(fromjson("{a: 5.4}")));
}

TEST(MatchExpressionParserLeafTest, TypeStringnameNumberLong) {
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression typeNumberLong = MatchExpressionParser::parse(
        fromjson("{a: {$type: 'long'}}"), ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_OK(typeNumberLong.getStatus());
    TypeMatchExpression* tmeNumberLong =
        static_cast<TypeMatchExpression*>(typeNumberLong.getValue().get());
    ASSERT(tmeNumberLong->getType() == NumberLong);
    ASSERT_TRUE(tmeNumberLong->matchesBSON(BSON("a" << -1LL)));
    ASSERT_FALSE(tmeNumberLong->matchesBSON(fromjson("{a: true}")));
}

TEST(MatchExpressionParserLeafTest, TypeStringnameString) {
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression typeString = MatchExpressionParser::parse(
        fromjson("{a: {$type: 'string'}}"), ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_OK(typeString.getStatus());
    TypeMatchExpression* tmeString = static_cast<TypeMatchExpression*>(typeString.getValue().get());
    ASSERT(tmeString->getType() == String);
    ASSERT_TRUE(tmeString->matchesBSON(fromjson("{a: 'hello world'}")));
    ASSERT_FALSE(tmeString->matchesBSON(fromjson("{a: 5.4}")));
}

TEST(MatchExpressionParserLeafTest, TypeStringnamejstOID) {
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression typejstOID = MatchExpressionParser::parse(
        fromjson("{a: {$type: 'objectId'}}"), ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_OK(typejstOID.getStatus());
    TypeMatchExpression* tmejstOID = static_cast<TypeMatchExpression*>(typejstOID.getValue().get());
    ASSERT(tmejstOID->getType() == jstOID);
    ASSERT_TRUE(tmejstOID->matchesBSON(fromjson("{a: ObjectId('000000000000000000000000')}")));
    ASSERT_FALSE(tmejstOID->matchesBSON(fromjson("{a: 'hello world'}")));
}

TEST(MatchExpressionParserLeafTest, TypeStringnamejstNULL) {
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression typejstNULL = MatchExpressionParser::parse(
        fromjson("{a: {$type: 'null'}}"), ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_OK(typejstNULL.getStatus());
    TypeMatchExpression* tmejstNULL =
        static_cast<TypeMatchExpression*>(typejstNULL.getValue().get());
    ASSERT(tmejstNULL->getType() == jstNULL);
    ASSERT_TRUE(tmejstNULL->matchesBSON(fromjson("{a: null}")));
    ASSERT_FALSE(tmejstNULL->matchesBSON(fromjson("{a: true}")));
}

TEST(MatchExpressionParserLeafTest, TypeStringnameBool) {
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression typeBool = MatchExpressionParser::parse(
        fromjson("{a: {$type: 'bool'}}"), ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_OK(typeBool.getStatus());
    TypeMatchExpression* tmeBool = static_cast<TypeMatchExpression*>(typeBool.getValue().get());
    ASSERT(tmeBool->getType() == Bool);
    ASSERT_TRUE(tmeBool->matchesBSON(fromjson("{a: true}")));
    ASSERT_FALSE(tmeBool->matchesBSON(fromjson("{a: null}")));
}

TEST(MatchExpressionParserLeafTest, TypeStringnameObject) {
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression typeObject = MatchExpressionParser::parse(
        fromjson("{a: {$type: 'object'}}"), ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_OK(typeObject.getStatus());
    TypeMatchExpression* tmeObject = static_cast<TypeMatchExpression*>(typeObject.getValue().get());
    ASSERT(tmeObject->getType() == Object);
    ASSERT_TRUE(tmeObject->matchesBSON(fromjson("{a: {}}")));
    ASSERT_FALSE(tmeObject->matchesBSON(fromjson("{a: []}")));
}

TEST(MatchExpressionParserLeafTest, TypeStringnameArray) {
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression typeArray = MatchExpressionParser::parse(
        fromjson("{a: {$type: 'array'}}"), ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_OK(typeArray.getStatus());
    TypeMatchExpression* tmeArray = static_cast<TypeMatchExpression*>(typeArray.getValue().get());
    ASSERT(tmeArray->getType() == Array);
    ASSERT_TRUE(tmeArray->matchesBSON(fromjson("{a: [[]]}")));
    ASSERT_FALSE(tmeArray->matchesBSON(fromjson("{a: {}}")));
}

TEST(MatchExpressionParserLeafTest, TypeStringnameNumber) {
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression typeNumber = MatchExpressionParser::parse(
        fromjson("{a: {$type: 'number'}}"), ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_OK(typeNumber.getStatus());
    TypeMatchExpression* tmeNumber = static_cast<TypeMatchExpression*>(typeNumber.getValue().get());
    ASSERT_TRUE(tmeNumber->matchesBSON(fromjson("{a: 5.4}")));
    ASSERT_TRUE(tmeNumber->matchesBSON(fromjson("{a: NumberInt(5)}")));
    ASSERT_TRUE(tmeNumber->matchesBSON(BSON("a" << -1LL)));
    ASSERT_FALSE(tmeNumber->matchesBSON(fromjson("{a: ''}")));
}

TEST(MatchExpressionParserLeafTest, InvalidTypeCodeLessThanMinKeyFailsToParse) {
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression typeNumber = MatchExpressionParser::parse(
        fromjson("{a: {$type: -20}}"), ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_NOT_OK(typeNumber.getStatus());
}

TEST(MatchExpressionParserLeafTest, InvalidTypeCodeGreaterThanMaxKeyFailsToParse) {
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression typeNumber = MatchExpressionParser::parse(
        fromjson("{a: {$type: 400}}"), ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_NOT_OK(typeNumber.getStatus());
}

TEST(MatchExpressionParserLeafTest, InvalidTypeCodeUnusedBetweenMinAndMaxFailsToParse) {
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression typeNumber = MatchExpressionParser::parse(
        fromjson("{a: {$type: 62}}"), ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_NOT_OK(typeNumber.getStatus());
}

TEST(MatchExpressionParserLeafTest, ValidTypeCodesParseSuccessfully) {
    std::vector<BSONType> validTypes{
        MinKey,    EOO,    NumberDouble, String,    Object,        Array,      BinData,
        Undefined, jstOID, Bool,         Date,      jstNULL,       RegEx,      DBRef,
        Code,      Symbol, CodeWScope,   NumberInt, bsonTimestamp, NumberLong, MaxKey};

    for (auto type : validTypes) {
        BSONObj predicate = BSON("a" << BSON("$type" << type));
        const CollatorInterface* collator = nullptr;
        auto expression = MatchExpressionParser::parse(
            predicate, ExtensionsCallbackDisallowExtensions(), collator);
        ASSERT_OK(expression.getStatus());
        auto typeExpression = static_cast<TypeMatchExpression*>(expression.getValue().get());
        ASSERT_EQ(type, typeExpression->getType());
    }
}

TEST(MatchExpressionParserTest, BitTestMatchExpressionValidMask) {
    const double k2Power53 = scalbn(1, 32);

    const CollatorInterface* collator = nullptr;
    ASSERT_OK(MatchExpressionParser::parse(BSON("a" << BSON("$bitsAllSet" << 54)),
                                           ExtensionsCallbackDisallowExtensions(),
                                           collator)
                  .getStatus());
    ASSERT_OK(MatchExpressionParser::parse(
                  BSON("a" << BSON("$bitsAllSet" << std::numeric_limits<long long>::max())),
                  ExtensionsCallbackDisallowExtensions(),
                  collator)
                  .getStatus());
    ASSERT_OK(MatchExpressionParser::parse(BSON("a" << BSON("$bitsAllSet" << k2Power53)),
                                           ExtensionsCallbackDisallowExtensions(),
                                           collator)
                  .getStatus());
    ASSERT_OK(MatchExpressionParser::parse(BSON("a" << BSON("$bitsAllSet" << k2Power53 - 1)),
                                           ExtensionsCallbackDisallowExtensions(),
                                           collator)
                  .getStatus());

    ASSERT_OK(MatchExpressionParser::parse(BSON("a" << BSON("$bitsAllClear" << 54)),
                                           ExtensionsCallbackDisallowExtensions(),
                                           collator)
                  .getStatus());
    ASSERT_OK(MatchExpressionParser::parse(
                  BSON("a" << BSON("$bitsAllClear" << std::numeric_limits<long long>::max())),
                  ExtensionsCallbackDisallowExtensions(),
                  collator)
                  .getStatus());
    ASSERT_OK(MatchExpressionParser::parse(BSON("a" << BSON("$bitsAllClear" << k2Power53)),
                                           ExtensionsCallbackDisallowExtensions(),
                                           collator)
                  .getStatus());
    ASSERT_OK(MatchExpressionParser::parse(BSON("a" << BSON("$bitsAllClear" << k2Power53 - 1)),
                                           ExtensionsCallbackDisallowExtensions(),
                                           collator)
                  .getStatus());

    ASSERT_OK(MatchExpressionParser::parse(BSON("a" << BSON("$bitsAnySet" << 54)),
                                           ExtensionsCallbackDisallowExtensions(),
                                           collator)
                  .getStatus());
    ASSERT_OK(MatchExpressionParser::parse(
                  BSON("a" << BSON("$bitsAnySet" << std::numeric_limits<long long>::max())),
                  ExtensionsCallbackDisallowExtensions(),
                  collator)
                  .getStatus());
    ASSERT_OK(MatchExpressionParser::parse(BSON("a" << BSON("$bitsAnySet" << k2Power53)),
                                           ExtensionsCallbackDisallowExtensions(),
                                           collator)
                  .getStatus());
    ASSERT_OK(MatchExpressionParser::parse(BSON("a" << BSON("$bitsAnySet" << k2Power53 - 1)),
                                           ExtensionsCallbackDisallowExtensions(),
                                           collator)
                  .getStatus());

    ASSERT_OK(MatchExpressionParser::parse(BSON("a" << BSON("$bitsAnyClear" << 54)),
                                           ExtensionsCallbackDisallowExtensions(),
                                           collator)
                  .getStatus());
    ASSERT_OK(MatchExpressionParser::parse(
                  BSON("a" << BSON("$bitsAnyClear" << std::numeric_limits<long long>::max())),
                  ExtensionsCallbackDisallowExtensions(),
                  collator)
                  .getStatus());
    ASSERT_OK(MatchExpressionParser::parse(BSON("a" << BSON("$bitsAnyClear" << k2Power53)),
                                           ExtensionsCallbackDisallowExtensions(),
                                           collator)
                  .getStatus());
    ASSERT_OK(MatchExpressionParser::parse(BSON("a" << BSON("$bitsAnyClear" << k2Power53 - 1)),
                                           ExtensionsCallbackDisallowExtensions(),
                                           collator)
                  .getStatus());
}

TEST(MatchExpressionParserTest, BitTestMatchExpressionValidArray) {
    BSONArray bsonArrayLongLong = BSON_ARRAY(0LL << 1LL << 2LL << 3LL);
    ASSERT_EQ(BSONType::NumberLong, bsonArrayLongLong[0].type());
    ASSERT_EQ(BSONType::NumberLong, bsonArrayLongLong[1].type());
    ASSERT_EQ(BSONType::NumberLong, bsonArrayLongLong[2].type());
    ASSERT_EQ(BSONType::NumberLong, bsonArrayLongLong[3].type());

    const CollatorInterface* collator = nullptr;
    ASSERT_OK(MatchExpressionParser::parse(BSON("a" << BSON("$bitsAllSet" << BSON_ARRAY(0))),
                                           ExtensionsCallbackDisallowExtensions(),
                                           collator)
                  .getStatus());
    ASSERT_OK(MatchExpressionParser::parse(
                  BSON("a" << BSON("$bitsAllSet" << BSON_ARRAY(0 << 1 << 2 << 3))),
                  ExtensionsCallbackDisallowExtensions(),
                  collator)
                  .getStatus());
    ASSERT_OK(MatchExpressionParser::parse(BSON("a" << BSON("$bitsAllSet" << bsonArrayLongLong)),
                                           ExtensionsCallbackDisallowExtensions(),
                                           collator)
                  .getStatus());
    ASSERT_OK(MatchExpressionParser::parse(
                  BSON("a" << BSON("$bitsAllSet" << BSON_ARRAY(std::numeric_limits<int>::max()))),
                  ExtensionsCallbackDisallowExtensions(),
                  collator)
                  .getStatus());

    ASSERT_OK(MatchExpressionParser::parse(BSON("a" << BSON("$bitsAllClear" << BSON_ARRAY(0))),
                                           ExtensionsCallbackDisallowExtensions(),
                                           collator)
                  .getStatus());
    ASSERT_OK(MatchExpressionParser::parse(
                  BSON("a" << BSON("$bitsAllClear" << BSON_ARRAY(0 << 1 << 2 << 3))),
                  ExtensionsCallbackDisallowExtensions(),
                  collator)
                  .getStatus());
    ASSERT_OK(MatchExpressionParser::parse(BSON("a" << BSON("$bitsAllClear" << bsonArrayLongLong)),
                                           ExtensionsCallbackDisallowExtensions(),
                                           collator)
                  .getStatus());
    ASSERT_OK(MatchExpressionParser::parse(
                  BSON("a" << BSON("$bitsAllClear" << BSON_ARRAY(std::numeric_limits<int>::max()))),
                  ExtensionsCallbackDisallowExtensions(),
                  collator)
                  .getStatus());

    ASSERT_OK(MatchExpressionParser::parse(BSON("a" << BSON("$bitsAnySet" << BSON_ARRAY(0))),
                                           ExtensionsCallbackDisallowExtensions(),
                                           collator)
                  .getStatus());
    ASSERT_OK(MatchExpressionParser::parse(
                  BSON("a" << BSON("$bitsAnySet" << BSON_ARRAY(0 << 1 << 2 << 3))),
                  ExtensionsCallbackDisallowExtensions(),
                  collator)
                  .getStatus());
    ASSERT_OK(MatchExpressionParser::parse(BSON("a" << BSON("$bitsAnySet" << bsonArrayLongLong)),
                                           ExtensionsCallbackDisallowExtensions(),
                                           collator)
                  .getStatus());
    ASSERT_OK(MatchExpressionParser::parse(
                  BSON("a" << BSON("$bitsAnySet" << BSON_ARRAY(std::numeric_limits<int>::max()))),
                  ExtensionsCallbackDisallowExtensions(),
                  collator)
                  .getStatus());

    ASSERT_OK(MatchExpressionParser::parse(BSON("a" << BSON("$bitsAnyClear" << BSON_ARRAY(0))),
                                           ExtensionsCallbackDisallowExtensions(),
                                           collator)
                  .getStatus());
    ASSERT_OK(MatchExpressionParser::parse(
                  BSON("a" << BSON("$bitsAnyClear" << BSON_ARRAY(0 << 1 << 2 << 3))),
                  ExtensionsCallbackDisallowExtensions(),
                  collator)
                  .getStatus());
    ASSERT_OK(MatchExpressionParser::parse(BSON("a" << BSON("$bitsAnyClear" << bsonArrayLongLong)),
                                           ExtensionsCallbackDisallowExtensions(),
                                           collator)
                  .getStatus());
    ASSERT_OK(MatchExpressionParser::parse(
                  BSON("a" << BSON("$bitsAnyClear" << BSON_ARRAY(std::numeric_limits<int>::max()))),
                  ExtensionsCallbackDisallowExtensions(),
                  collator)
                  .getStatus());
}

TEST(MatchExpressionParserTest, BitTestMatchExpressionValidBinData) {
    const CollatorInterface* collator = nullptr;
    ASSERT_OK(
        MatchExpressionParser::parse(
            fromjson("{a: {$bitsAllSet: {$binary: 'AAAAAAAAAAAAAAAAAAAAAAAAAAAA', $type: '00'}}}"),
            ExtensionsCallbackDisallowExtensions(),
            collator)
            .getStatus());

    ASSERT_OK(
        MatchExpressionParser::parse(
            fromjson(
                "{a: {$bitsAllClear: {$binary: 'AAAAAAAAAAAAAAAAAAAAAAAAAAAA', $type: '00'}}}"),
            ExtensionsCallbackDisallowExtensions(),
            collator)
            .getStatus());

    ASSERT_OK(
        MatchExpressionParser::parse(
            fromjson("{a: {$bitsAnySet: {$binary: 'AAAAAAAAAAAAAAAAAAAAAAAAAAAA', $type: '00'}}}"),
            ExtensionsCallbackDisallowExtensions(),
            collator)
            .getStatus());

    ASSERT_OK(
        MatchExpressionParser::parse(
            fromjson(
                "{a: {$bitsAnyClear: {$binary: 'AAAAAAAAAAAAAAAAAAAAAAAAAAAA', $type: '00'}}}"),
            ExtensionsCallbackDisallowExtensions(),
            collator)
            .getStatus());
}

TEST(MatchExpressionParserTest, BitTestMatchExpressionInvalidMaskType) {
    const CollatorInterface* collator = nullptr;
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAllSet: null}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAllSet: true}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAllSet: {}}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAllSet: ''}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());

    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAllClear: null}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAllClear: true}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAllClear: {}}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAllClear: ''}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(
                      fromjson("{a: {$bitsAllClear: ObjectId('000000000000000000000000')}}"),
                      ExtensionsCallbackDisallowExtensions(),
                      collator)
                      .getStatus());

    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAnySet: null}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAnySet: true}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAnySet: {}}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAnySet: ''}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(
                      fromjson("{a: {$bitsAnySet: ObjectId('000000000000000000000000')}}"),
                      ExtensionsCallbackDisallowExtensions(),
                      collator)
                      .getStatus());

    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAnyClear: null}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAnyClear: true}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAnyClear: {}}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAnyClear: ''}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(
                      fromjson("{a: {$bitsAnyClear: ObjectId('000000000000000000000000')}}"),
                      ExtensionsCallbackDisallowExtensions(),
                      collator)
                      .getStatus());
}

TEST(MatchExpressionParserTest, BitTestMatchExpressionInvalidMaskValue) {
    const double kLongLongMaxAsDouble = scalbn(1, std::numeric_limits<long long>::digits);

    const CollatorInterface* collator = nullptr;
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAllSet: NaN}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAllSet: -54}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(
                      BSON("a" << BSON("$bitsAllSet" << std::numeric_limits<double>::max())),
                      ExtensionsCallbackDisallowExtensions(),
                      collator)
                      .getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(BSON("a" << BSON("$bitsAllSet" << kLongLongMaxAsDouble)),
                                     ExtensionsCallbackDisallowExtensions(),
                                     collator)
            .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAllSet: 2.5}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());

    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAllClear: NaN}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAllClear: -54}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(
                      BSON("a" << BSON("$bitsAllClear" << std::numeric_limits<double>::max())),
                      ExtensionsCallbackDisallowExtensions(),
                      collator)
                      .getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(BSON("a" << BSON("$bitsAllClear" << kLongLongMaxAsDouble)),
                                     ExtensionsCallbackDisallowExtensions(),
                                     collator)
            .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAllClear: 2.5}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());

    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAnySet: NaN}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAnySet: -54}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(
                      BSON("a" << BSON("$bitsAnySet" << std::numeric_limits<double>::max())),
                      ExtensionsCallbackDisallowExtensions(),
                      collator)
                      .getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(BSON("a" << BSON("$bitsAnySet" << kLongLongMaxAsDouble)),
                                     ExtensionsCallbackDisallowExtensions(),
                                     collator)
            .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAnySet: 2.5}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());

    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAnyClear: NaN}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAnyClear: -54}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(
                      BSON("a" << BSON("$bitsAnyClear" << std::numeric_limits<double>::max())),
                      ExtensionsCallbackDisallowExtensions(),
                      collator)
                      .getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(BSON("a" << BSON("$bitsAnyClear" << kLongLongMaxAsDouble)),
                                     ExtensionsCallbackDisallowExtensions(),
                                     collator)
            .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAnyClear: 2.5}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
}

TEST(MatchExpressionParserTest, BitTestMatchExpressionInvalidArray) {
    const CollatorInterface* collator = nullptr;
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAllSet: [null]}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAllSet: [true]}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAllSet: ['']}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAllSet: [{}]}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAllSet: [[]]}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAllSet: [-1]}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAllSet: [2.5]}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(
            fromjson(
                "{a: {$bitsAllSet: [{$binary: 'AAAAAAAAAAAAAAAAAAAAAAAAAAAA', $type: '00'}]}}"),
            ExtensionsCallbackDisallowExtensions(),
            collator)
            .getStatus());

    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAllClear: [null]}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAllClear: [true]}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAllClear: ['']}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAllClear: [{}]}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAllClear: [[]]}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAllClear: [-1]}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAllClear: [2.5]}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(
            fromjson(
                "{a: {$bitsAllClear: [{$binary: 'AAAAAAAAAAAAAAAAAAAAAAAAAAAA', $type: '00'}]}}"),
            ExtensionsCallbackDisallowExtensions(),
            collator)
            .getStatus());

    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAnySet: [null]}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAnySet: [true]}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAnySet: ['']}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAnySet: [{}]}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAnySet: [[]]}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAnySet: [-1]}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAnySet: [2.5]}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(
            fromjson(
                "{a: {$bitsAnySet: [{$binary: 'AAAAAAAAAAAAAAAAAAAAAAAAAAAA', $type: '00'}]}}"),
            ExtensionsCallbackDisallowExtensions(),
            collator)
            .getStatus());

    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAnyClear: [null]}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAnyClear: [true]}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAnyClear: ['']}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAnyClear: [{}]}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAnyClear: [[]]}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAnyClear: [-1]}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAnyClear: [2.5]}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(
            fromjson(
                "{a: {$bitsAnyClear: [{$binary: 'AAAAAAAAAAAAAAAAAAAAAAAAAAAA', $type: '00'}]}}"),
            ExtensionsCallbackDisallowExtensions(),
            collator)
            .getStatus());
}

TEST(MatchExpressionParserTest, BitTestMatchExpressionInvalidArrayValue) {
    const CollatorInterface* collator = nullptr;
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAllSet: [-54]}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAllSet: [NaN]}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAllSet: [2.5]}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAllSet: [1e100]}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAllSet: [-1e100]}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(
            BSON("a" << BSON("$bitsAllSet" << BSON_ARRAY(std::numeric_limits<long long>::max()))),
            ExtensionsCallbackDisallowExtensions(),
            collator)
            .getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(
            BSON("a" << BSON("$bitsAllSet" << BSON_ARRAY(std::numeric_limits<long long>::min()))),
            ExtensionsCallbackDisallowExtensions(),
            collator)
            .getStatus());

    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAllClear: [-54]}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAllClear: [NaN]}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAllClear: [2.5]}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAllClear: [1e100]}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAllClear: [-1e100]}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(
            BSON("a" << BSON("$bitsAllClear" << BSON_ARRAY(std::numeric_limits<long long>::max()))),
            ExtensionsCallbackDisallowExtensions(),
            collator)
            .getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(
            BSON("a" << BSON("$bitsAllClear" << BSON_ARRAY(std::numeric_limits<long long>::min()))),
            ExtensionsCallbackDisallowExtensions(),
            collator)
            .getStatus());

    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAnySet: [-54]}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAnySet: [NaN]}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAnySet: [2.5]}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAnySet: [1e100]}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAnySet: [-1e100]}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(
            BSON("a" << BSON("$bitsAnySet" << BSON_ARRAY(std::numeric_limits<long long>::max()))),
            ExtensionsCallbackDisallowExtensions(),
            collator)
            .getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(
            BSON("a" << BSON("$bitsAnySet" << BSON_ARRAY(std::numeric_limits<long long>::min()))),
            ExtensionsCallbackDisallowExtensions(),
            collator)
            .getStatus());

    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAnyClear: [-54]}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAnyClear: [NaN]}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAnyClear: [2.5]}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAnyClear: [1e100]}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAnyClear: [-1e100]}}"),
                                               ExtensionsCallbackDisallowExtensions(),
                                               collator)
                      .getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(
            BSON("a" << BSON("$bitsAnyClear" << BSON_ARRAY(std::numeric_limits<long long>::max()))),
            ExtensionsCallbackDisallowExtensions(),
            collator)
            .getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(
            BSON("a" << BSON("$bitsAnyClear" << BSON_ARRAY(std::numeric_limits<long long>::min()))),
            ExtensionsCallbackDisallowExtensions(),
            collator)
            .getStatus());
}
}
