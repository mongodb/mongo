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

#include "mongo/base/checked_cast.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_type.h"
#include "mongo/db/matcher/matcher_type_set.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/intrusive_counter.h"

#include <cmath>
#include <limits>
#include <memory>
#include <ostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

using std::string;

TEST(MatchExpressionParserLeafTest, NullCollation) {
    BSONObj query = BSON("x" << "string");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(MatchExpression::EQ, result.getValue()->matchType());
    EqualityMatchExpression* match = static_cast<EqualityMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == nullptr);
}


TEST(MatchExpressionParserLeafTest, Collation) {
    BSONObj query = BSON("x" << "string");
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kAlwaysEqual);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(std::move(collator));
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(MatchExpression::EQ, result.getValue()->matchType());
    EqualityMatchExpression* match = static_cast<EqualityMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == expCtx->getCollator());
}

TEST(MatchExpressionParserLeafTest, SimpleEQUndefined) {
    BSONObj query = BSON("x" << BSON("$eq" << BSONUndefined));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(MatchExpressionParserLeafTest, EQNullCollation) {
    BSONObj query = BSON("x" << BSON("$eq" << "string"));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(MatchExpression::EQ, result.getValue()->matchType());
    EqualityMatchExpression* match = static_cast<EqualityMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == nullptr);
}


TEST(MatchExpressionParserLeafTest, EQCollation) {
    BSONObj query = BSON("x" << BSON("$eq" << "string"));
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kAlwaysEqual);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(std::move(collator));
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(MatchExpression::EQ, result.getValue()->matchType());
    EqualityMatchExpression* match = static_cast<EqualityMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == expCtx->getCollator());
}

TEST(MatchExpressionParserLeafTest, GTNullCollation) {
    BSONObj query = BSON("x" << BSON("$gt" << "abc"));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(MatchExpression::GT, result.getValue()->matchType());
    GTMatchExpression* match = static_cast<GTMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == nullptr);
}


TEST(MatchExpressionParserLeafTest, GTCollation) {
    BSONObj query = BSON("x" << BSON("$gt" << "abc"));
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kReverseString);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(std::move(collator));
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(MatchExpression::GT, result.getValue()->matchType());
    GTMatchExpression* match = static_cast<GTMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == expCtx->getCollator());
}

TEST(MatchExpressionParserLeafTest, LTNullCollation) {
    BSONObj query = BSON("x" << BSON("$lt" << "abc"));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(MatchExpression::LT, result.getValue()->matchType());
    LTMatchExpression* match = static_cast<LTMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == nullptr);
}


TEST(MatchExpressionParserLeafTest, LTCollation) {
    BSONObj query = BSON("x" << BSON("$lt" << "abc"));
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kReverseString);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(std::move(collator));
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(MatchExpression::LT, result.getValue()->matchType());
    LTMatchExpression* match = static_cast<LTMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == expCtx->getCollator());
}

TEST(MatchExpressionParserLeafTest, GTENullCollation) {
    BSONObj query = BSON("x" << BSON("$gte" << "abc"));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(MatchExpression::GTE, result.getValue()->matchType());
    GTEMatchExpression* match = static_cast<GTEMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == nullptr);
}


TEST(MatchExpressionParserLeafTest, GTECollation) {
    BSONObj query = BSON("x" << BSON("$gte" << "abc"));
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kReverseString);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(std::move(collator));
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(MatchExpression::GTE, result.getValue()->matchType());
    GTEMatchExpression* match = static_cast<GTEMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == expCtx->getCollator());
}

TEST(MatchExpressionParserLeafTest, LTENullCollation) {
    BSONObj query = BSON("x" << BSON("$lte" << "abc"));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(MatchExpression::LTE, result.getValue()->matchType());
    LTEMatchExpression* match = static_cast<LTEMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == nullptr);
}


TEST(MatchExpressionParserLeafTest, LTECollation) {
    BSONObj query = BSON("x" << BSON("$lte" << "abc"));
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kReverseString);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(std::move(collator));
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(MatchExpression::LTE, result.getValue()->matchType());
    LTEMatchExpression* match = static_cast<LTEMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == expCtx->getCollator());
}

TEST(MatchExpressionParserLeafTest, NENullCollation) {
    BSONObj query = BSON("x" << BSON("$ne" << "string"));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(MatchExpression::NOT, result.getValue()->matchType());
    MatchExpression* child = result.getValue()->getChild(0);
    ASSERT_EQUALS(MatchExpression::EQ, child->matchType());
    EqualityMatchExpression* eqMatch = static_cast<EqualityMatchExpression*>(child);
    ASSERT_TRUE(eqMatch->getCollator() == nullptr);
}


TEST(MatchExpressionParserLeafTest, NECollation) {
    BSONObj query = BSON("x" << BSON("$ne" << "string"));
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kAlwaysEqual);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(std::move(collator));
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(MatchExpression::NOT, result.getValue()->matchType());
    MatchExpression* child = result.getValue()->getChild(0);
    ASSERT_EQUALS(MatchExpression::EQ, child->matchType());
    EqualityMatchExpression* eqMatch = static_cast<EqualityMatchExpression*>(child);
    ASSERT_TRUE(eqMatch->getCollator() == expCtx->getCollator());
}

TEST(MatchExpressionParserLeafTest, SimpleModBad1) {
    BSONObj query = BSON("x" << BSON("$mod" << BSON_ARRAY(3 << 2)));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    query = BSON("x" << BSON("$mod" << BSON_ARRAY(3)));
    result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_NOT_OK(result.getStatus());

    query = BSON("x" << BSON("$mod" << BSON_ARRAY(3 << 2 << 4)));
    result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_NOT_OK(result.getStatus());

    query = BSON("x" << BSON("$mod" << BSON_ARRAY("q" << 2)));
    result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_NOT_OK(result.getStatus());

    query = BSON("x" << BSON("$mod" << 3));
    result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_NOT_OK(result.getStatus());

    query = BSON("x" << BSON("$mod" << BSON("a" << 1 << "b" << 2)));
    result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_NOT_OK(result.getStatus());

    query = BSON("x" << BSON("$mod" << BSON_ARRAY(5 << "r")));
    result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_NOT_OK(result.getStatus());

    query = BSON("x" << BSON("$mod" << BSON_ARRAY(5 << BSONNULL)));
    result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(MatchExpressionParserLeafTest, ModFloatTruncate) {
    struct TestCase {
        BSONObj _query;
        long long _divider;
        long long _remainder;
    };

    const auto positiveLargerThanInt = 3 * static_cast<long long>(std::numeric_limits<int>::max());
    const auto negativeSmallerThanInt = 3 * static_cast<long long>(std::numeric_limits<int>::min());
    std::vector<TestCase> testCases = {
        {BSON("x" << BSON("$mod" << BSON_ARRAY(3 << 2))), 3, 2},
        {BSON("x" << BSON("$mod" << BSON_ARRAY(3LL << 2LL))), 3, 2},
        {BSON("x" << BSON("$mod" << BSON_ARRAY(3.2 << 2.2))), 3, 2},
        {BSON("x" << BSON("$mod" << BSON_ARRAY(3.7 << 2.7))), 3, 2},
        {BSON("x" << BSON("$mod" << BSON_ARRAY(Decimal128("3") << Decimal128("2")))), 3, 2},
        {BSON("x" << BSON("$mod" << BSON_ARRAY(Decimal128("3.2") << Decimal128("2.2")))), 3, 2},
        {BSON("x" << BSON("$mod" << BSON_ARRAY(Decimal128("3.7") << Decimal128("2.7")))), 3, 2},
        {BSON("x" << BSON("$mod" << BSON_ARRAY(positiveLargerThanInt << positiveLargerThanInt))),
         positiveLargerThanInt,
         positiveLargerThanInt},
        {BSON("x" << BSON("$mod" << BSON_ARRAY(static_cast<double>(positiveLargerThanInt)
                                               << static_cast<double>(positiveLargerThanInt)))),
         positiveLargerThanInt,
         positiveLargerThanInt},
        {BSON("x" << BSON("$mod" << BSON_ARRAY(Decimal128(positiveLargerThanInt)
                                               << Decimal128(positiveLargerThanInt)))),
         positiveLargerThanInt,
         positiveLargerThanInt},

        {BSON("x" << BSON("$mod" << BSON_ARRAY(-3 << -2))), -3, -2},
        {BSON("x" << BSON("$mod" << BSON_ARRAY(-3LL << -2LL))), -3, -2},
        {BSON("x" << BSON("$mod" << BSON_ARRAY(-3.2 << -2.2))), -3, -2},
        {BSON("x" << BSON("$mod" << BSON_ARRAY(-3.7 << -2.7))), -3, -2},
        {BSON("x" << BSON("$mod" << BSON_ARRAY(Decimal128("-3") << Decimal128("-2")))), -3, -2},
        {BSON("x" << BSON("$mod" << BSON_ARRAY(Decimal128("-3.2") << Decimal128("-2.2")))), -3, -2},
        {BSON("x" << BSON("$mod" << BSON_ARRAY(Decimal128("-3.7") << Decimal128("-2.7")))), -3, -2},
        {BSON("x" << BSON("$mod" << BSON_ARRAY(negativeSmallerThanInt << negativeSmallerThanInt))),
         negativeSmallerThanInt,
         negativeSmallerThanInt},
        {BSON("x" << BSON("$mod" << BSON_ARRAY(static_cast<double>(negativeSmallerThanInt)
                                               << static_cast<double>(negativeSmallerThanInt)))),
         negativeSmallerThanInt,
         negativeSmallerThanInt},
        {BSON("x" << BSON("$mod" << BSON_ARRAY(Decimal128(negativeSmallerThanInt)
                                               << Decimal128(negativeSmallerThanInt)))),
         negativeSmallerThanInt,
         negativeSmallerThanInt},
    };

    for (const auto& testCase : testCases) {
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        StatusWithMatchExpression result = MatchExpressionParser::parse(testCase._query, expCtx);
        ASSERT_OK(result.getStatus());
        auto modExpr = checked_cast<ModMatchExpression*>(result.getValue().get());
        ASSERT_EQ(modExpr->getDivisor(), testCase._divider);
        ASSERT_EQ(modExpr->getRemainder(), testCase._remainder);
    }
}

TEST(MatchExpressionParserLeafTest, IdCollation) {
    BSONObj query = BSON("$id" << "string");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(MatchExpression::EQ, result.getValue()->matchType());
    EqualityMatchExpression* match = static_cast<EqualityMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == nullptr);
}

TEST(MatchExpressionParserLeafTest, IdNullCollation) {
    BSONObj query = BSON("$id" << "string");
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kAlwaysEqual);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(std::move(collator));
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(MatchExpression::EQ, result.getValue()->matchType());
    EqualityMatchExpression* match = static_cast<EqualityMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == expCtx->getCollator());
}

TEST(MatchExpressionParserLeafTest, RefCollation) {
    BSONObj query = BSON("$ref" << "coll");
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kAlwaysEqual);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(std::move(collator));
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(MatchExpression::EQ, result.getValue()->matchType());
    EqualityMatchExpression* match = static_cast<EqualityMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == nullptr);
}

TEST(MatchExpressionParserLeafTest, DbCollation) {
    BSONObj query = BSON("$db" << "db");
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kAlwaysEqual);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(std::move(collator));
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(MatchExpression::EQ, result.getValue()->matchType());
    EqualityMatchExpression* match = static_cast<EqualityMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == nullptr);
}

TEST(MatchExpressionParserLeafTest, INNullCollation) {
    BSONObj query = BSON("x" << BSON("$in" << BSON_ARRAY("string")));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(MatchExpression::MATCH_IN, result.getValue()->matchType());
    InMatchExpression* match = static_cast<InMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == nullptr);
}

TEST(MatchExpressionParserLeafTest, INCollation) {
    BSONObj query = BSON("x" << BSON("$in" << BSON_ARRAY("string")));
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kAlwaysEqual);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(std::move(collator));
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(MatchExpression::MATCH_IN, result.getValue()->matchType());
    InMatchExpression* match = static_cast<InMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == expCtx->getCollator());
}

TEST(MatchExpressionParserLeafTest, INInvalidDBRefs) {
    // missing $id
    BSONObj query = BSON("x" << BSON("$in" << BSON_ARRAY(BSON("$ref" << "coll"))));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    result = MatchExpressionParser::parse(query, expCtx);

    // second field is not $id
    query = BSON("x" << BSON("$in" << BSON_ARRAY(BSON("$ref" << "coll"
                                                             << "$foo" << 1))));
    result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_NOT_OK(result.getStatus());

    OID oid = OID::gen();

    // missing $ref field
    query = BSON("x" << BSON("$in" << BSON_ARRAY(BSON("$id" << oid << "foo" << 3))));
    result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_NOT_OK(result.getStatus());

    // missing $id and $ref field
    query = BSON("x" << BSON("$in" << BSON_ARRAY(BSON("$db" << "test"
                                                            << "foo" << 3))));
    result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(MatchExpressionParserLeafTest, INExpressionDocument) {
    BSONObj query = BSON("x" << BSON("$in" << BSON_ARRAY(BSON("$foo" << 1))));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(MatchExpressionParserLeafTest, INNotArray) {
    BSONObj query = BSON("x" << BSON("$in" << 5));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(MatchExpressionParserLeafTest, INUndefined) {
    BSONObj query = BSON("x" << BSON("$in" << BSON_ARRAY(BSONUndefined)));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(MatchExpressionParserLeafTest, INNotElemMatch) {
    BSONObj query = BSON("x" << BSON("$in" << BSON_ARRAY(BSON("$elemMatch" << 1))));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(MatchExpressionParserLeafTest, INRegexTooLong) {
    string tooLargePattern(50 * 1000, 'z');
    BSONObjBuilder inArray;
    inArray.appendRegex("0", tooLargePattern, "");
    BSONObjBuilder operand;
    operand.appendArray("$in", inArray.obj());
    BSONObj query = BSON("x" << operand.obj());
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(MatchExpressionParserLeafTest, INRegexTooLong2) {
    string tooLargePattern(50 * 1000, 'z');
    BSONObj query = BSON("x" << BSON("$in" << BSON_ARRAY(BSON("$regex" << tooLargePattern))));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(MatchExpressionParserLeafTest, NINNotArray) {
    BSONObj query = BSON("x" << BSON("$nin" << 5));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(MatchExpressionParserLeafTest, NINNullCollation) {
    BSONObj query = BSON("x" << BSON("$nin" << BSON_ARRAY("string")));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(MatchExpression::NOT, result.getValue()->matchType());
    MatchExpression* child = result.getValue()->getChild(0);
    ASSERT_EQUALS(MatchExpression::MATCH_IN, child->matchType());
    InMatchExpression* inMatch = static_cast<InMatchExpression*>(child);
    ASSERT_TRUE(inMatch->getCollator() == nullptr);
}

TEST(MatchExpressionParserLeafTest, NINCollation) {
    BSONObj query = BSON("x" << BSON("$nin" << BSON_ARRAY("string")));
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kAlwaysEqual);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(std::move(collator));
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(MatchExpression::NOT, result.getValue()->matchType());
    MatchExpression* child = result.getValue()->getChild(0);
    ASSERT_EQUALS(MatchExpression::MATCH_IN, child->matchType());
    InMatchExpression* inMatch = static_cast<InMatchExpression*>(child);
    ASSERT_TRUE(inMatch->getCollator() == expCtx->getCollator());
}

TEST(MatchExpressionParserLeafTest, RegexBad) {
    BSONObj query = BSON("x" << BSON("$regex" << "abc"
                                              << "$optionas"
                                              << "i"));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_NOT_OK(result.getStatus());

    // $regex does not with numbers
    query = BSON("x" << BSON("$regex" << 123));
    result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_NOT_OK(result.getStatus());

    query = BSON("x" << BSON("$regex" << BSON_ARRAY("abc")));
    result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_NOT_OK(result.getStatus());

    query = BSON("x" << BSON("$optionas" << "i"));
    result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_NOT_OK(result.getStatus());

    query = BSON("x" << BSON("$options" << "i"));
    result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(MatchExpressionParserLeafTest, TypeDoubleOperatorFailsToParse) {
    BSONObj query = BSON("x" << BSON("$type" << 1.5));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQ(ErrorCodes::BadValue, result.getStatus());
}

TEST(MatchExpressionParserLeafTest, TypeBadType) {
    BSONObjBuilder b;
    b.append("$type", (stdx::to_underlying(BSONType::jsTypeMax) + 1));
    BSONObj query = BSON("x" << b.obj());
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(MatchExpressionParserLeafTest, TypeBad) {
    BSONObj query = BSON("x" << BSON("$type" << BSON("x" << 1)));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(MatchExpressionParserLeafTest, TypeBadString) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$type: null}}"), expCtx).getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$type: true}}"), expCtx).getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$type: {}}}"), expCtx).getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(
                      fromjson("{a: {$type: ObjectId('000000000000000000000000')}}"), expCtx)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$type: []}}"), expCtx).getStatus());
}

TEST(MatchExpressionParserLeafTest, CanParseArrayOfTypes) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression typeNumber =
        MatchExpressionParser::parse(fromjson("{a: {$type: ['number', 2, 'object']}}"), expCtx);
    ASSERT_OK(typeNumber.getStatus());
    TypeMatchExpression* tme = static_cast<TypeMatchExpression*>(typeNumber.getValue().get());
    ASSERT_TRUE(tme->typeSet().allNumbers);
    ASSERT_EQ(tme->typeSet().bsonTypes.size(), 2u);
    ASSERT_TRUE(tme->typeSet().hasType(BSONType::string));
    ASSERT_TRUE(tme->typeSet().hasType(BSONType::object));
}

TEST(MatchExpressionParserLeafTest, EmptyArrayFailsToParse) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression typeNumber =
        MatchExpressionParser::parse(fromjson("{a: {$type: []}}"), expCtx);
    ASSERT_NOT_OK(typeNumber.getStatus());
}

TEST(MatchExpressionParserLeafTest, InvalidTypeCodeLessThanMinKeyFailsToParse) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression typeNumber =
        MatchExpressionParser::parse(fromjson("{a: {$type: -20}}"), expCtx);
    ASSERT_NOT_OK(typeNumber.getStatus());
}

TEST(MatchExpressionParserLeafTest, InvalidTypeCodeGreaterThanMaxKeyFailsToParse) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression typeNumber =
        MatchExpressionParser::parse(fromjson("{a: {$type: 400}}"), expCtx);
    ASSERT_NOT_OK(typeNumber.getStatus());
}

TEST(MatchExpressionParserLeafTest, InvalidTypeCodeUnusedBetweenMinAndMaxFailsToParse) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression typeNumber =
        MatchExpressionParser::parse(fromjson("{a: {$type: 62}}"), expCtx);
    ASSERT_NOT_OK(typeNumber.getStatus());
}

TEST(MatchExpressionParserLeafTest, ValidTypeCodesParseSuccessfully) {
    std::vector<BSONType> validTypes{
        BSONType::minKey,    BSONType::numberDouble, BSONType::string,     BSONType::object,
        BSONType::array,     BSONType::binData,      BSONType::undefined,  BSONType::oid,
        BSONType::boolean,   BSONType::date,         BSONType::null,       BSONType::regEx,
        BSONType::dbRef,     BSONType::code,         BSONType::symbol,     BSONType::codeWScope,
        BSONType::numberInt, BSONType::timestamp,    BSONType::numberLong, BSONType::maxKey,
    };

    for (auto type : validTypes) {
        BSONObj predicate = BSON("a" << BSON("$type" << type));
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        auto expression = MatchExpressionParser::parse(predicate, expCtx);
        ASSERT_OK(expression.getStatus());
        auto typeExpression = static_cast<TypeMatchExpression*>(expression.getValue().get());
        ASSERT_FALSE(typeExpression->typeSet().allNumbers);
        ASSERT_EQ(typeExpression->typeSet().bsonTypes.size(), 1u);
        ASSERT_TRUE(typeExpression->typeSet().isSingleType());
        ASSERT_TRUE(typeExpression->typeSet().hasType(type));
    }
}

TEST(MatchExpressionParserTest, BitTestMatchExpressionValidMask) {
    const double k2Power53 = scalbn(1, 32);

    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_OK(
        MatchExpressionParser::parse(BSON("a" << BSON("$bitsAllSet" << 54)), expCtx).getStatus());
    ASSERT_OK(MatchExpressionParser::parse(
                  BSON("a" << BSON("$bitsAllSet" << std::numeric_limits<long long>::max())), expCtx)
                  .getStatus());
    ASSERT_OK(MatchExpressionParser::parse(BSON("a" << BSON("$bitsAllSet" << k2Power53)), expCtx)
                  .getStatus());
    ASSERT_OK(
        MatchExpressionParser::parse(BSON("a" << BSON("$bitsAllSet" << k2Power53 - 1)), expCtx)
            .getStatus());

    ASSERT_OK(
        MatchExpressionParser::parse(BSON("a" << BSON("$bitsAllClear" << 54)), expCtx).getStatus());
    ASSERT_OK(
        MatchExpressionParser::parse(
            BSON("a" << BSON("$bitsAllClear" << std::numeric_limits<long long>::max())), expCtx)
            .getStatus());
    ASSERT_OK(MatchExpressionParser::parse(BSON("a" << BSON("$bitsAllClear" << k2Power53)), expCtx)
                  .getStatus());
    ASSERT_OK(
        MatchExpressionParser::parse(BSON("a" << BSON("$bitsAllClear" << k2Power53 - 1)), expCtx)
            .getStatus());

    ASSERT_OK(
        MatchExpressionParser::parse(BSON("a" << BSON("$bitsAnySet" << 54)), expCtx).getStatus());
    ASSERT_OK(MatchExpressionParser::parse(
                  BSON("a" << BSON("$bitsAnySet" << std::numeric_limits<long long>::max())), expCtx)
                  .getStatus());
    ASSERT_OK(MatchExpressionParser::parse(BSON("a" << BSON("$bitsAnySet" << k2Power53)), expCtx)
                  .getStatus());
    ASSERT_OK(
        MatchExpressionParser::parse(BSON("a" << BSON("$bitsAnySet" << k2Power53 - 1)), expCtx)
            .getStatus());

    ASSERT_OK(
        MatchExpressionParser::parse(BSON("a" << BSON("$bitsAnyClear" << 54)), expCtx).getStatus());
    ASSERT_OK(
        MatchExpressionParser::parse(
            BSON("a" << BSON("$bitsAnyClear" << std::numeric_limits<long long>::max())), expCtx)
            .getStatus());
    ASSERT_OK(MatchExpressionParser::parse(BSON("a" << BSON("$bitsAnyClear" << k2Power53)), expCtx)
                  .getStatus());
    ASSERT_OK(
        MatchExpressionParser::parse(BSON("a" << BSON("$bitsAnyClear" << k2Power53 - 1)), expCtx)
            .getStatus());
}

TEST(MatchExpressionParserTest, BitTestMatchExpressionValidArray) {
    BSONArray bsonArrayLongLong = BSON_ARRAY(0LL << 1LL << 2LL << 3LL);
    ASSERT_EQ(BSONType::numberLong, bsonArrayLongLong[0].type());
    ASSERT_EQ(BSONType::numberLong, bsonArrayLongLong[1].type());
    ASSERT_EQ(BSONType::numberLong, bsonArrayLongLong[2].type());
    ASSERT_EQ(BSONType::numberLong, bsonArrayLongLong[3].type());

    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_OK(
        MatchExpressionParser::parse(BSON("a" << BSON("$bitsAllSet" << BSON_ARRAY(0))), expCtx)
            .getStatus());
    ASSERT_OK(MatchExpressionParser::parse(
                  BSON("a" << BSON("$bitsAllSet" << BSON_ARRAY(0 << 1 << 2 << 3))), expCtx)
                  .getStatus());
    ASSERT_OK(
        MatchExpressionParser::parse(BSON("a" << BSON("$bitsAllSet" << bsonArrayLongLong)), expCtx)
            .getStatus());
    ASSERT_OK(
        MatchExpressionParser::parse(
            BSON("a" << BSON("$bitsAllSet" << BSON_ARRAY(std::numeric_limits<int>::max()))), expCtx)
            .getStatus());

    ASSERT_OK(
        MatchExpressionParser::parse(BSON("a" << BSON("$bitsAllClear" << BSON_ARRAY(0))), expCtx)
            .getStatus());
    ASSERT_OK(MatchExpressionParser::parse(
                  BSON("a" << BSON("$bitsAllClear" << BSON_ARRAY(0 << 1 << 2 << 3))), expCtx)
                  .getStatus());
    ASSERT_OK(MatchExpressionParser::parse(BSON("a" << BSON("$bitsAllClear" << bsonArrayLongLong)),
                                           expCtx)
                  .getStatus());
    ASSERT_OK(MatchExpressionParser::parse(
                  BSON("a" << BSON("$bitsAllClear" << BSON_ARRAY(std::numeric_limits<int>::max()))),
                  expCtx)
                  .getStatus());

    ASSERT_OK(
        MatchExpressionParser::parse(BSON("a" << BSON("$bitsAnySet" << BSON_ARRAY(0))), expCtx)
            .getStatus());
    ASSERT_OK(MatchExpressionParser::parse(
                  BSON("a" << BSON("$bitsAnySet" << BSON_ARRAY(0 << 1 << 2 << 3))), expCtx)
                  .getStatus());
    ASSERT_OK(
        MatchExpressionParser::parse(BSON("a" << BSON("$bitsAnySet" << bsonArrayLongLong)), expCtx)
            .getStatus());
    ASSERT_OK(
        MatchExpressionParser::parse(
            BSON("a" << BSON("$bitsAnySet" << BSON_ARRAY(std::numeric_limits<int>::max()))), expCtx)
            .getStatus());

    ASSERT_OK(
        MatchExpressionParser::parse(BSON("a" << BSON("$bitsAnyClear" << BSON_ARRAY(0))), expCtx)
            .getStatus());
    ASSERT_OK(MatchExpressionParser::parse(
                  BSON("a" << BSON("$bitsAnyClear" << BSON_ARRAY(0 << 1 << 2 << 3))), expCtx)
                  .getStatus());
    ASSERT_OK(MatchExpressionParser::parse(BSON("a" << BSON("$bitsAnyClear" << bsonArrayLongLong)),
                                           expCtx)
                  .getStatus());
    ASSERT_OK(MatchExpressionParser::parse(
                  BSON("a" << BSON("$bitsAnyClear" << BSON_ARRAY(std::numeric_limits<int>::max()))),
                  expCtx)
                  .getStatus());
}

TEST(MatchExpressionParserTest, BitTestMatchExpressionValidBinData) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_OK(
        MatchExpressionParser::parse(
            fromjson("{a: {$bitsAllSet: {$binary: 'AAAAAAAAAAAAAAAAAAAAAAAAAAAA', $type: '00'}}}"),
            expCtx)
            .getStatus());

    ASSERT_OK(
        MatchExpressionParser::parse(
            fromjson(
                "{a: {$bitsAllClear: {$binary: 'AAAAAAAAAAAAAAAAAAAAAAAAAAAA', $type: '00'}}}"),
            expCtx)
            .getStatus());

    ASSERT_OK(
        MatchExpressionParser::parse(
            fromjson("{a: {$bitsAnySet: {$binary: 'AAAAAAAAAAAAAAAAAAAAAAAAAAAA', $type: '00'}}}"),
            expCtx)
            .getStatus());

    ASSERT_OK(
        MatchExpressionParser::parse(
            fromjson(
                "{a: {$bitsAnyClear: {$binary: 'AAAAAAAAAAAAAAAAAAAAAAAAAAAA', $type: '00'}}}"),
            expCtx)
            .getStatus());
}

TEST(MatchExpressionParserTest, BitTestMatchExpressionInvalidMaskType) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAllSet: null}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAllSet: true}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAllSet: {}}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAllSet: ''}}"), expCtx).getStatus());

    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAllClear: null}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAllClear: true}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAllClear: {}}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAllClear: ''}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(
            fromjson("{a: {$bitsAllClear: ObjectId('000000000000000000000000')}}"), expCtx)
            .getStatus());

    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAnySet: null}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAnySet: true}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAnySet: {}}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAnySet: ''}}"), expCtx).getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(
                      fromjson("{a: {$bitsAnySet: ObjectId('000000000000000000000000')}}"), expCtx)
                      .getStatus());

    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAnyClear: null}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAnyClear: true}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAnyClear: {}}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAnyClear: ''}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(
            fromjson("{a: {$bitsAnyClear: ObjectId('000000000000000000000000')}}"), expCtx)
            .getStatus());
}

TEST(MatchExpressionParserTest, BitTestMatchExpressionInvalidMaskValue) {
    const double kLongLongMaxAsDouble = scalbn(1, std::numeric_limits<long long>::digits);

    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAllSet: NaN}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAllSet: -54}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(
            BSON("a" << BSON("$bitsAllSet" << std::numeric_limits<double>::max())), expCtx)
            .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(
                      BSON("a" << BSON("$bitsAllSet" << kLongLongMaxAsDouble)), expCtx)
                      .getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAllSet: 2.5}}"), expCtx).getStatus());

    ASSERT_NOT_OK(
        MatchExpressionParser::parse(BSON("a" << BSON("$bitsAllSet" << Decimal128("2.5"))), expCtx)
            .getStatus());

    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAllClear: NaN}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAllClear: -54}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(
            BSON("a" << BSON("$bitsAllClear" << std::numeric_limits<double>::max())), expCtx)
            .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(
                      BSON("a" << BSON("$bitsAllClear" << kLongLongMaxAsDouble)), expCtx)
                      .getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAllClear: 2.5}}"), expCtx).getStatus());

    ASSERT_NOT_OK(MatchExpressionParser::parse(
                      BSON("a" << BSON("$bitsAllClear" << Decimal128("2.5"))), expCtx)
                      .getStatus());

    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAnySet: NaN}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAnySet: -54}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(
            BSON("a" << BSON("$bitsAnySet" << std::numeric_limits<double>::max())), expCtx)
            .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(
                      BSON("a" << BSON("$bitsAnySet" << kLongLongMaxAsDouble)), expCtx)
                      .getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAnySet: 2.5}}"), expCtx).getStatus());

    ASSERT_NOT_OK(
        MatchExpressionParser::parse(BSON("a" << BSON("$bitsAnySet" << Decimal128("2.5"))), expCtx)
            .getStatus());

    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAnyClear: NaN}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAnyClear: -54}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(
            BSON("a" << BSON("$bitsAnyClear" << std::numeric_limits<double>::max())), expCtx)
            .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(
                      BSON("a" << BSON("$bitsAnyClear" << kLongLongMaxAsDouble)), expCtx)
                      .getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAnyClear: 2.5}}"), expCtx).getStatus());

    ASSERT_NOT_OK(MatchExpressionParser::parse(
                      BSON("a" << BSON("$bitsAnyClear" << Decimal128("2.5"))), expCtx)
                      .getStatus());
}

TEST(MatchExpressionParserTest, BitTestMatchExpressionInvalidArray) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAllSet: [null]}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAllSet: [true]}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAllSet: ['']}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAllSet: [{}]}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAllSet: [[]]}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAllSet: [-1]}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAllSet: [2.5]}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(
            fromjson(
                "{a: {$bitsAllSet: [{$binary: 'AAAAAAAAAAAAAAAAAAAAAAAAAAAA', $type: '00'}]}}"),
            expCtx)
            .getStatus());

    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAllClear: [null]}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAllClear: [true]}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAllClear: ['']}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAllClear: [{}]}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAllClear: [[]]}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAllClear: [-1]}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAllClear: [2.5]}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(
            fromjson(
                "{a: {$bitsAllClear: [{$binary: 'AAAAAAAAAAAAAAAAAAAAAAAAAAAA', $type: '00'}]}}"),
            expCtx)
            .getStatus());

    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAnySet: [null]}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAnySet: [true]}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAnySet: ['']}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAnySet: [{}]}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAnySet: [[]]}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAnySet: [-1]}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAnySet: [2.5]}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(
            fromjson(
                "{a: {$bitsAnySet: [{$binary: 'AAAAAAAAAAAAAAAAAAAAAAAAAAAA', $type: '00'}]}}"),
            expCtx)
            .getStatus());

    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAnyClear: [null]}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAnyClear: [true]}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAnyClear: ['']}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAnyClear: [{}]}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAnyClear: [[]]}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAnyClear: [-1]}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAnyClear: [2.5]}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(
            fromjson(
                "{a: {$bitsAnyClear: [{$binary: 'AAAAAAAAAAAAAAAAAAAAAAAAAAAA', $type: '00'}]}}"),
            expCtx)
            .getStatus());
}

TEST(MatchExpressionParserTest, BitTestMatchExpressionInvalidArrayValue) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAllSet: [-54]}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAllSet: [NaN]}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAllSet: [2.5]}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAllSet: [1e100]}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAllSet: [-1e100]}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(
            BSON("a" << BSON("$bitsAllSet" << BSON_ARRAY(std::numeric_limits<long long>::max()))),
            expCtx)
            .getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(
            BSON("a" << BSON("$bitsAllSet" << BSON_ARRAY(std::numeric_limits<long long>::min()))),
            expCtx)
            .getStatus());

    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAllClear: [-54]}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAllClear: [NaN]}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAllClear: [2.5]}}"), expCtx).getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAllClear: [1e100]}}"), expCtx)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAllClear: [-1e100]}}"), expCtx)
                      .getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(
            BSON("a" << BSON("$bitsAllClear" << BSON_ARRAY(std::numeric_limits<long long>::max()))),
            expCtx)
            .getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(
            BSON("a" << BSON("$bitsAllClear" << BSON_ARRAY(std::numeric_limits<long long>::min()))),
            expCtx)
            .getStatus());

    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAnySet: [-54]}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAnySet: [NaN]}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAnySet: [2.5]}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAnySet: [1e100]}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAnySet: [-1e100]}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(
            BSON("a" << BSON("$bitsAnySet" << BSON_ARRAY(std::numeric_limits<long long>::max()))),
            expCtx)
            .getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(
            BSON("a" << BSON("$bitsAnySet" << BSON_ARRAY(std::numeric_limits<long long>::min()))),
            expCtx)
            .getStatus());

    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAnyClear: [-54]}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAnyClear: [NaN]}}"), expCtx).getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(fromjson("{a: {$bitsAnyClear: [2.5]}}"), expCtx).getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAnyClear: [1e100]}}"), expCtx)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$bitsAnyClear: [-1e100]}}"), expCtx)
                      .getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(
            BSON("a" << BSON("$bitsAnyClear" << BSON_ARRAY(std::numeric_limits<long long>::max()))),
            expCtx)
            .getStatus());
    ASSERT_NOT_OK(
        MatchExpressionParser::parse(
            BSON("a" << BSON("$bitsAnyClear" << BSON_ARRAY(std::numeric_limits<long long>::min()))),
            expCtx)
            .getStatus());
}
}  // namespace mongo
