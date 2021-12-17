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

#include "mongo/platform/basic.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

using std::endl;
using std::string;

TEST(MatchExpressionParserLeafTest, NullCollation) {
    BSONObj query = BSON("x"
                         << "string");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(MatchExpression::EQ, result.getValue()->matchType());
    EqualityMatchExpression* match = static_cast<EqualityMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == nullptr);
}


TEST(MatchExpressionParserLeafTest, Collation) {
    BSONObj query = BSON("x"
                         << "string");
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(&collator);
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(MatchExpression::EQ, result.getValue()->matchType());
    EqualityMatchExpression* match = static_cast<EqualityMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == &collator);
}

TEST(MatchExpressionParserLeafTest, SimpleEQ2) {
    BSONObj query = BSON("x" << BSON("$eq" << 2));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 1)));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << 2)));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 3)));
}

TEST(MatchExpressionParserLeafTest, SimpleEQUndefined) {
    BSONObj query = BSON("x" << BSON("$eq" << BSONUndefined));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(MatchExpressionParserLeafTest, EQNullCollation) {
    BSONObj query = BSON("x" << BSON("$eq"
                                     << "string"));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(MatchExpression::EQ, result.getValue()->matchType());
    EqualityMatchExpression* match = static_cast<EqualityMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == nullptr);
}


TEST(MatchExpressionParserLeafTest, EQCollation) {
    BSONObj query = BSON("x" << BSON("$eq"
                                     << "string"));
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(&collator);
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(MatchExpression::EQ, result.getValue()->matchType());
    EqualityMatchExpression* match = static_cast<EqualityMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == &collator);
}

TEST(MatchExpressionParserLeafTest, SimpleGT1) {
    BSONObj query = BSON("x" << BSON("$gt" << 2));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 2)));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << 3)));
}

TEST(MatchExpressionParserLeafTest, GTNullCollation) {
    BSONObj query = BSON("x" << BSON("$gt"
                                     << "abc"));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(MatchExpression::GT, result.getValue()->matchType());
    GTMatchExpression* match = static_cast<GTMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == nullptr);
}


TEST(MatchExpressionParserLeafTest, GTCollation) {
    BSONObj query = BSON("x" << BSON("$gt"
                                     << "abc"));
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(&collator);
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(MatchExpression::GT, result.getValue()->matchType());
    GTMatchExpression* match = static_cast<GTMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == &collator);
}

TEST(MatchExpressionParserLeafTest, SimpleLT1) {
    BSONObj query = BSON("x" << BSON("$lt" << 2));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    ASSERT(result.getValue()->matchesBSON(BSON("x" << 1)));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 2)));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 3)));
}

TEST(MatchExpressionParserLeafTest, LTNullCollation) {
    BSONObj query = BSON("x" << BSON("$lt"
                                     << "abc"));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(MatchExpression::LT, result.getValue()->matchType());
    LTMatchExpression* match = static_cast<LTMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == nullptr);
}


TEST(MatchExpressionParserLeafTest, LTCollation) {
    BSONObj query = BSON("x" << BSON("$lt"
                                     << "abc"));
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(&collator);
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(MatchExpression::LT, result.getValue()->matchType());
    LTMatchExpression* match = static_cast<LTMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == &collator);
}

TEST(MatchExpressionParserLeafTest, SimpleGTE1) {
    BSONObj query = BSON("x" << BSON("$gte" << 2));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 1)));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << 2)));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << 3)));
}

TEST(MatchExpressionParserLeafTest, GTENullCollation) {
    BSONObj query = BSON("x" << BSON("$gte"
                                     << "abc"));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(MatchExpression::GTE, result.getValue()->matchType());
    GTEMatchExpression* match = static_cast<GTEMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == nullptr);
}


TEST(MatchExpressionParserLeafTest, GTECollation) {
    BSONObj query = BSON("x" << BSON("$gte"
                                     << "abc"));
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(&collator);
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(MatchExpression::GTE, result.getValue()->matchType());
    GTEMatchExpression* match = static_cast<GTEMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == &collator);
}

TEST(MatchExpressionParserLeafTest, SimpleLTE1) {
    BSONObj query = BSON("x" << BSON("$lte" << 2));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    ASSERT(result.getValue()->matchesBSON(BSON("x" << 1)));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << 2)));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 3)));
}

TEST(MatchExpressionParserLeafTest, LTENullCollation) {
    BSONObj query = BSON("x" << BSON("$lte"
                                     << "abc"));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(MatchExpression::LTE, result.getValue()->matchType());
    LTEMatchExpression* match = static_cast<LTEMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == nullptr);
}


TEST(MatchExpressionParserLeafTest, LTECollation) {
    BSONObj query = BSON("x" << BSON("$lte"
                                     << "abc"));
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(&collator);
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(MatchExpression::LTE, result.getValue()->matchType());
    LTEMatchExpression* match = static_cast<LTEMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == &collator);
}

TEST(MatchExpressionParserLeafTest, SimpleNE1) {
    BSONObj query = BSON("x" << BSON("$ne" << 2));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    ASSERT(result.getValue()->matchesBSON(BSON("x" << 1)));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 2)));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << 3)));
}

TEST(MatchExpressionParserLeafTest, NENullCollation) {
    BSONObj query = BSON("x" << BSON("$ne"
                                     << "string"));
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
    BSONObj query = BSON("x" << BSON("$ne"
                                     << "string"));
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(&collator);
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(MatchExpression::NOT, result.getValue()->matchType());
    MatchExpression* child = result.getValue()->getChild(0);
    ASSERT_EQUALS(MatchExpression::EQ, child->matchType());
    EqualityMatchExpression* eqMatch = static_cast<EqualityMatchExpression*>(child);
    ASSERT_TRUE(eqMatch->getCollator() == &collator);
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

TEST(MatchExpressionParserLeafTest, SimpleMod1) {
    BSONObj query = BSON("x" << BSON("$mod" << BSON_ARRAY(3 << 2)));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    ASSERT(result.getValue()->matchesBSON(BSON("x" << 5)));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 4)));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << 8)));
}

TEST(MatchExpressionParserLeafTest, ModFloatTruncate) {
    struct TestCase {
        BSONObj _query;
        long long _divider;
        long long _remainder;
    };

    const auto positiveLargerThanInt = 3 * static_cast<int64_t>(std::numeric_limits<int>::max());
    const auto negativeSmallerThanInt = 3 * static_cast<int64_t>(std::numeric_limits<int>::min());
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
    BSONObj query = BSON("$id"
                         << "string");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(MatchExpression::EQ, result.getValue()->matchType());
    EqualityMatchExpression* match = static_cast<EqualityMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == nullptr);
}

TEST(MatchExpressionParserLeafTest, IdNullCollation) {
    BSONObj query = BSON("$id"
                         << "string");
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(&collator);
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(MatchExpression::EQ, result.getValue()->matchType());
    EqualityMatchExpression* match = static_cast<EqualityMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == &collator);
}

TEST(MatchExpressionParserLeafTest, RefCollation) {
    BSONObj query = BSON("$ref"
                         << "coll");
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(&collator);
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(MatchExpression::EQ, result.getValue()->matchType());
    EqualityMatchExpression* match = static_cast<EqualityMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == nullptr);
}

TEST(MatchExpressionParserLeafTest, DbCollation) {
    BSONObj query = BSON("$db"
                         << "db");
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(&collator);
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(MatchExpression::EQ, result.getValue()->matchType());
    EqualityMatchExpression* match = static_cast<EqualityMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == nullptr);
}

TEST(MatchExpressionParserLeafTest, SimpleIN1) {
    BSONObj query = BSON("x" << BSON("$in" << BSON_ARRAY(2 << 3)));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 1)));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << 2)));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << 3)));
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
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(&collator);
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());
    ASSERT_EQUALS(MatchExpression::MATCH_IN, result.getValue()->matchType());
    InMatchExpression* match = static_cast<InMatchExpression*>(result.getValue().get());
    ASSERT_TRUE(match->getCollator() == &collator);
}

TEST(MatchExpressionParserLeafTest, INSingleDBRef) {
    OID oid = OID::gen();
    BSONObj query = BSON("x" << BSON("$in" << BSON_ARRAY(BSON("$ref"
                                                              << "coll"
                                                              << "$id" << oid << "$db"
                                                              << "db"))));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    OID oidx = OID::gen();
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON("$ref"
                                                            << "collx"
                                                            << "$id" << oidx << "$db"
                                                            << "db"))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON("$ref"
                                                            << "coll"
                                                            << "$id" << oidx << "$db"
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
                                                            << "$id" << oid << "$db"
                                                            << "dbx"))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON("$db"
                                                            << "db"
                                                            << "$ref"
                                                            << "coll"
                                                            << "$id" << oid))));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON("$ref"
                                                           << "coll"
                                                           << "$id" << oid << "$db"
                                                           << "db"))));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(BSON("$ref"
                                                                      << "coll"
                                                                      << "$id" << oid << "$db"
                                                                      << "db")))));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(BSON("$ref"
                                                                      << "collx"
                                                                      << "$id" << oidx << "$db"
                                                                      << "db")
                                                                 << BSON("$ref"
                                                                         << "coll"
                                                                         << "$id" << oid << "$db"
                                                                         << "db")))));
}

TEST(MatchExpressionParserLeafTest, INMultipleDBRef) {
    OID oid = OID::gen();
    OID oidy = OID::gen();
    BSONObj query = BSON("x" << BSON("$in" << BSON_ARRAY(BSON("$ref"
                                                              << "colly"
                                                              << "$id" << oidy << "$db"
                                                              << "db")
                                                         << BSON("$ref"
                                                                 << "coll"
                                                                 << "$id" << oid << "$db"
                                                                 << "db"))));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    OID oidx = OID::gen();
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON("$ref"
                                                            << "collx"
                                                            << "$id" << oidx << "$db"
                                                            << "db"))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON("$ref"
                                                            << "coll"
                                                            << "$id" << oidx << "$db"
                                                            << "db"))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON("$id" << oid << "$ref"
                                                                  << "coll"
                                                                  << "$db"
                                                                  << "db"))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(BSON("$ref"
                                                                       << "coll"
                                                                       << "$id" << oidy << "$db"
                                                                       << "db")))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(BSON("$ref"
                                                                       << "colly"
                                                                       << "$id" << oid << "$db"
                                                                       << "db")))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(BSON("$id" << oid << "$ref"
                                                                             << "coll"
                                                                             << "$db"
                                                                             << "db")))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(BSON("$ref"
                                                                       << "coll"
                                                                       << "$id" << oid << "$db"
                                                                       << "dbx")))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(BSON("$id" << oidy << "$ref"
                                                                             << "colly"
                                                                             << "$db"
                                                                             << "db")))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(BSON("$ref"
                                                                       << "collx"
                                                                       << "$id" << oidx << "$db"
                                                                       << "db")
                                                                  << BSON("$ref"
                                                                          << "coll"
                                                                          << "$id" << oidx << "$db"
                                                                          << "db")))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(BSON("$ref"
                                                                       << "collx"
                                                                       << "$id" << oidx << "$db"
                                                                       << "db")
                                                                  << BSON("$ref"
                                                                          << "colly"
                                                                          << "$id" << oidx << "$db"
                                                                          << "db")))));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(BSON("$ref"
                                                                       << "collx"
                                                                       << "$id" << oidx << "$db"
                                                                       << "db")
                                                                  << BSON("$ref"
                                                                          << "coll"
                                                                          << "$id" << oid << "$db"
                                                                          << "dbx")))));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON("$ref"
                                                           << "coll"
                                                           << "$id" << oid << "$db"
                                                           << "db"))));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON("$ref"
                                                           << "colly"
                                                           << "$id" << oidy << "$db"
                                                           << "db"))));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(BSON("$ref"
                                                                      << "coll"
                                                                      << "$id" << oid << "$db"
                                                                      << "db")))));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(BSON("$ref"
                                                                      << "colly"
                                                                      << "$id" << oidy << "$db"
                                                                      << "db")))));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(BSON("$ref"
                                                                      << "collx"
                                                                      << "$id" << oidx << "$db"
                                                                      << "db")
                                                                 << BSON("$ref"
                                                                         << "coll"
                                                                         << "$id" << oid << "$db"
                                                                         << "db")))));
    ASSERT(result.getValue()->matchesBSON(BSON("x" << BSON_ARRAY(BSON("$ref"
                                                                      << "collx"
                                                                      << "$id" << oidx << "$db"
                                                                      << "db")
                                                                 << BSON("$ref"
                                                                         << "colly"
                                                                         << "$id" << oidy << "$db"
                                                                         << "db")))));
}

TEST(MatchExpressionParserLeafTest, INDBRefWithOptionalField1) {
    OID oid = OID::gen();
    BSONObj query = BSON("x" << BSON("$in" << BSON_ARRAY(BSON("$ref"
                                                              << "coll"
                                                              << "$id" << oid << "foo" << 12345))));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    OID oidx = OID::gen();
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << BSON("$ref"
                                                            << "coll"
                                                            << "$id" << oidx << "$db"
                                                            << "db"))));
    ASSERT(result.getValue()->matchesBSON(
        BSON("x" << BSON_ARRAY(BSON("$ref"
                                    << "coll"
                                    << "$id" << oid << "foo" << 12345)))));
    ASSERT(result.getValue()->matchesBSON(
        BSON("x" << BSON_ARRAY(BSON("$ref"
                                    << "collx"
                                    << "$id" << oidx << "foo" << 12345)
                               << BSON("$ref"
                                       << "coll"
                                       << "$id" << oid << "foo" << 12345)))));
}

TEST(MatchExpressionParserLeafTest, INInvalidDBRefs) {
    // missing $id
    BSONObj query = BSON("x" << BSON("$in" << BSON_ARRAY(BSON("$ref"
                                                              << "coll"))));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    result = MatchExpressionParser::parse(query, expCtx);

    // second field is not $id
    query = BSON("x" << BSON("$in" << BSON_ARRAY(BSON("$ref"
                                                      << "coll"
                                                      << "$foo" << 1))));
    result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_NOT_OK(result.getStatus());

    OID oid = OID::gen();

    // missing $ref field
    query = BSON("x" << BSON("$in" << BSON_ARRAY(BSON("$id" << oid << "foo" << 3))));
    result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_NOT_OK(result.getStatus());

    // missing $id and $ref field
    query = BSON("x" << BSON("$in" << BSON_ARRAY(BSON("$db"
                                                      << "test"
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

TEST(MatchExpressionParserLeafTest, INRegexStuff) {
    BSONObjBuilder inArray;
    inArray.appendRegex("0", "^a", "");
    inArray.appendRegex("1", "B", "i");
    inArray.append("2", 4);
    BSONObjBuilder operand;
    operand.appendArray("$in", inArray.obj());

    BSONObj query = BSON("a" << operand.obj());
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

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
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    ASSERT(result.getValue()->matchesBSON(BSON("x" << 1)));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 2)));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 3)));
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
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(&collator);
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());
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
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

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
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

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
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

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

    query = BSON("x" << BSON("$optionas"
                             << "i"));
    result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_NOT_OK(result.getStatus());

    query = BSON("x" << BSON("$options"
                             << "i"));
    result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_NOT_OK(result.getStatus());
}

TEST(MatchExpressionParserLeafTest, RegexEmbeddedNULByte) {
    BSONObj query = BSON("x" << BSON("$regex"
                                     << "^a\\x00b"));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    const auto value = "a\0b"_sd;
    ASSERT(result.getValue()->matchesBSON(BSON("x" << value)));
    ASSERT(!result.getValue()->matchesBSON(BSON("x"
                                                << "a")));
}

TEST(MatchExpressionParserLeafTest, ExistsYes1) {
    BSONObjBuilder b;
    b.appendBool("$exists", true);
    BSONObj query = BSON("x" << b.obj());
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    ASSERT(result.getValue()->matchesBSON(BSON("x"
                                               << "abc")));
    ASSERT(!result.getValue()->matchesBSON(BSON("y"
                                                << "AC")));
}

TEST(MatchExpressionParserLeafTest, ExistsNO1) {
    BSONObjBuilder b;
    b.appendBool("$exists", false);
    BSONObj query = BSON("x" << b.obj());
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    ASSERT(!result.getValue()->matchesBSON(BSON("x"
                                                << "abc")));
    ASSERT(result.getValue()->matchesBSON(BSON("y"
                                               << "AC")));
}

TEST(MatchExpressionParserLeafTest, Type1) {
    BSONObj query = BSON("x" << BSON("$type" << String));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    ASSERT(result.getValue()->matchesBSON(BSON("x"
                                               << "abc")));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 5)));
}

TEST(MatchExpressionParserLeafTest, Type2) {
    BSONObj query = BSON("x" << BSON("$type" << (double)NumberDouble));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    ASSERT(result.getValue()->matchesBSON(BSON("x" << 5.3)));
    ASSERT(!result.getValue()->matchesBSON(BSON("x" << 5)));
}

TEST(MatchExpressionParserLeafTest, TypeDoubleOperatorFailsToParse) {
    BSONObj query = BSON("x" << BSON("$type" << 1.5));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQ(ErrorCodes::BadValue, result.getStatus());
}

TEST(MatchExpressionParserLeafTest, TypeDecimalOperator) {
    BSONObj query = BSON("x" << BSON("$type" << mongo::NumberDecimal));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

    ASSERT_FALSE(result.getValue()->matchesBSON(BSON("x" << 5.3)));
    ASSERT_TRUE(result.getValue()->matchesBSON(BSON("x" << mongo::Decimal128("1"))));
}

TEST(MatchExpressionParserLeafTest, TypeNull) {
    BSONObj query = BSON("x" << BSON("$type" << jstNULL));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_OK(result.getStatus());

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
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$type: {}}}}"), expCtx).getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(
                      fromjson("{a: {$type: ObjectId('000000000000000000000000')}}"), expCtx)
                      .getStatus());
    ASSERT_NOT_OK(MatchExpressionParser::parse(fromjson("{a: {$type: []}}"), expCtx).getStatus());
}

TEST(MatchExpressionParserLeafTest, TypeStringnameDouble) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression typeNumberDouble =
        MatchExpressionParser::parse(fromjson("{a: {$type: 'double'}}"), expCtx);
    ASSERT_OK(typeNumberDouble.getStatus());
    TypeMatchExpression* tmeNumberDouble =
        static_cast<TypeMatchExpression*>(typeNumberDouble.getValue().get());
    ASSERT_FALSE(tmeNumberDouble->typeSet().allNumbers);
    ASSERT_EQ(tmeNumberDouble->typeSet().bsonTypes.size(), 1u);
    ASSERT_TRUE(tmeNumberDouble->typeSet().hasType(BSONType::NumberDouble));
    ASSERT_TRUE(tmeNumberDouble->matchesBSON(fromjson("{a: 5.4}")));
    ASSERT_FALSE(tmeNumberDouble->matchesBSON(fromjson("{a: NumberInt(5)}")));
}

TEST(MatchExpressionParserLeafTest, TypeStringNameNumberDecimal) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression typeNumberDecimal =
        MatchExpressionParser::parse(fromjson("{a: {$type: 'decimal'}}"), expCtx);
    ASSERT_OK(typeNumberDecimal.getStatus());
    TypeMatchExpression* tmeNumberDecimal =
        static_cast<TypeMatchExpression*>(typeNumberDecimal.getValue().get());
    ASSERT_FALSE(tmeNumberDecimal->typeSet().allNumbers);
    ASSERT_EQ(tmeNumberDecimal->typeSet().bsonTypes.size(), 1u);
    ASSERT_TRUE(tmeNumberDecimal->typeSet().hasType(BSONType::NumberDecimal));
    ASSERT_TRUE(tmeNumberDecimal->matchesBSON(BSON("a" << mongo::Decimal128("1"))));
    ASSERT_FALSE(tmeNumberDecimal->matchesBSON(fromjson("{a: true}")));
}

TEST(MatchExpressionParserLeafTest, TypeStringnameNumberInt) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression typeNumberInt =
        MatchExpressionParser::parse(fromjson("{a: {$type: 'int'}}"), expCtx);
    ASSERT_OK(typeNumberInt.getStatus());
    TypeMatchExpression* tmeNumberInt =
        static_cast<TypeMatchExpression*>(typeNumberInt.getValue().get());
    ASSERT_FALSE(tmeNumberInt->typeSet().allNumbers);
    ASSERT_EQ(tmeNumberInt->typeSet().bsonTypes.size(), 1u);
    ASSERT_TRUE(tmeNumberInt->typeSet().hasType(BSONType::NumberInt));
    ASSERT_TRUE(tmeNumberInt->matchesBSON(fromjson("{a: NumberInt(5)}")));
    ASSERT_FALSE(tmeNumberInt->matchesBSON(fromjson("{a: 5.4}")));
}

TEST(MatchExpressionParserLeafTest, TypeStringnameNumberLong) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression typeNumberLong =
        MatchExpressionParser::parse(fromjson("{a: {$type: 'long'}}"), expCtx);
    ASSERT_OK(typeNumberLong.getStatus());
    TypeMatchExpression* tmeNumberLong =
        static_cast<TypeMatchExpression*>(typeNumberLong.getValue().get());
    ASSERT_FALSE(tmeNumberLong->typeSet().allNumbers);
    ASSERT_EQ(tmeNumberLong->typeSet().bsonTypes.size(), 1u);
    ASSERT_TRUE(tmeNumberLong->typeSet().hasType(BSONType::NumberLong));
    ASSERT_TRUE(tmeNumberLong->matchesBSON(BSON("a" << -1LL)));
    ASSERT_FALSE(tmeNumberLong->matchesBSON(fromjson("{a: true}")));
}

TEST(MatchExpressionParserLeafTest, TypeStringnameString) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression typeString =
        MatchExpressionParser::parse(fromjson("{a: {$type: 'string'}}"), expCtx);
    ASSERT_OK(typeString.getStatus());
    TypeMatchExpression* tmeString = static_cast<TypeMatchExpression*>(typeString.getValue().get());
    ASSERT_FALSE(tmeString->typeSet().allNumbers);
    ASSERT_EQ(tmeString->typeSet().bsonTypes.size(), 1u);
    ASSERT_TRUE(tmeString->typeSet().hasType(BSONType::String));
    ASSERT_TRUE(tmeString->matchesBSON(fromjson("{a: 'hello world'}")));
    ASSERT_FALSE(tmeString->matchesBSON(fromjson("{a: 5.4}")));
}

TEST(MatchExpressionParserLeafTest, TypeStringnamejstOID) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression typejstOID =
        MatchExpressionParser::parse(fromjson("{a: {$type: 'objectId'}}"), expCtx);
    ASSERT_OK(typejstOID.getStatus());
    TypeMatchExpression* tmejstOID = static_cast<TypeMatchExpression*>(typejstOID.getValue().get());
    ASSERT_FALSE(tmejstOID->typeSet().allNumbers);
    ASSERT_EQ(tmejstOID->typeSet().bsonTypes.size(), 1u);
    ASSERT_TRUE(tmejstOID->typeSet().hasType(BSONType::jstOID));
    ASSERT_TRUE(tmejstOID->matchesBSON(fromjson("{a: ObjectId('000000000000000000000000')}")));
    ASSERT_FALSE(tmejstOID->matchesBSON(fromjson("{a: 'hello world'}")));
}

TEST(MatchExpressionParserLeafTest, TypeStringnamejstNULL) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression typejstNULL =
        MatchExpressionParser::parse(fromjson("{a: {$type: 'null'}}"), expCtx);
    ASSERT_OK(typejstNULL.getStatus());
    TypeMatchExpression* tmejstNULL =
        static_cast<TypeMatchExpression*>(typejstNULL.getValue().get());
    ASSERT_FALSE(tmejstNULL->typeSet().allNumbers);
    ASSERT_EQ(tmejstNULL->typeSet().bsonTypes.size(), 1u);
    ASSERT_TRUE(tmejstNULL->typeSet().hasType(BSONType::jstNULL));
    ASSERT_TRUE(tmejstNULL->matchesBSON(fromjson("{a: null}")));
    ASSERT_FALSE(tmejstNULL->matchesBSON(fromjson("{a: true}")));
}

TEST(MatchExpressionParserLeafTest, TypeStringnameBool) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression typeBool =
        MatchExpressionParser::parse(fromjson("{a: {$type: 'bool'}}"), expCtx);
    ASSERT_OK(typeBool.getStatus());
    TypeMatchExpression* tmeBool = static_cast<TypeMatchExpression*>(typeBool.getValue().get());
    ASSERT_FALSE(tmeBool->typeSet().allNumbers);
    ASSERT_EQ(tmeBool->typeSet().bsonTypes.size(), 1u);
    ASSERT_TRUE(tmeBool->typeSet().hasType(BSONType::Bool));
    ASSERT_TRUE(tmeBool->matchesBSON(fromjson("{a: true}")));
    ASSERT_FALSE(tmeBool->matchesBSON(fromjson("{a: null}")));
}

TEST(MatchExpressionParserLeafTest, TypeStringnameObject) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression typeObject =
        MatchExpressionParser::parse(fromjson("{a: {$type: 'object'}}"), expCtx);
    ASSERT_OK(typeObject.getStatus());
    TypeMatchExpression* tmeObject = static_cast<TypeMatchExpression*>(typeObject.getValue().get());
    ASSERT_FALSE(tmeObject->typeSet().allNumbers);
    ASSERT_EQ(tmeObject->typeSet().bsonTypes.size(), 1u);
    ASSERT_TRUE(tmeObject->typeSet().hasType(BSONType::Object));
    ASSERT_TRUE(tmeObject->matchesBSON(fromjson("{a: {}}")));
    ASSERT_FALSE(tmeObject->matchesBSON(fromjson("{a: []}")));
}

TEST(MatchExpressionParserLeafTest, TypeStringnameArray) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression typeArray =
        MatchExpressionParser::parse(fromjson("{a: {$type: 'array'}}"), expCtx);
    ASSERT_OK(typeArray.getStatus());
    TypeMatchExpression* tmeArray = static_cast<TypeMatchExpression*>(typeArray.getValue().get());
    ASSERT_FALSE(tmeArray->typeSet().allNumbers);
    ASSERT_EQ(tmeArray->typeSet().bsonTypes.size(), 1u);
    ASSERT_TRUE(tmeArray->typeSet().hasType(BSONType::Array));
    ASSERT_TRUE(tmeArray->matchesBSON(fromjson("{a: [[]]}")));
    ASSERT_FALSE(tmeArray->matchesBSON(fromjson("{a: {}}")));
}

TEST(MatchExpressionParserLeafTest, TypeStringnameNumber) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression typeNumber =
        MatchExpressionParser::parse(fromjson("{a: {$type: 'number'}}"), expCtx);
    ASSERT_OK(typeNumber.getStatus());
    TypeMatchExpression* tmeNumber = static_cast<TypeMatchExpression*>(typeNumber.getValue().get());
    ASSERT_TRUE(tmeNumber->typeSet().allNumbers);
    ASSERT_EQ(tmeNumber->typeSet().bsonTypes.size(), 0u);
    ASSERT_TRUE(tmeNumber->matchesBSON(fromjson("{a: 5.4}")));
    ASSERT_TRUE(tmeNumber->matchesBSON(fromjson("{a: NumberInt(5)}")));
    ASSERT_TRUE(tmeNumber->matchesBSON(BSON("a" << -1LL)));
    ASSERT_FALSE(tmeNumber->matchesBSON(fromjson("{a: ''}")));
}

TEST(MatchExpressionParserLeafTest, CanParseArrayOfTypes) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression typeNumber =
        MatchExpressionParser::parse(fromjson("{a: {$type: ['number', 2, 'object']}}"), expCtx);
    ASSERT_OK(typeNumber.getStatus());
    TypeMatchExpression* tme = static_cast<TypeMatchExpression*>(typeNumber.getValue().get());
    ASSERT_TRUE(tme->typeSet().allNumbers);
    ASSERT_EQ(tme->typeSet().bsonTypes.size(), 2u);
    ASSERT_TRUE(tme->typeSet().hasType(BSONType::String));
    ASSERT_TRUE(tme->typeSet().hasType(BSONType::Object));
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
    std::vector<BSONType> validTypes{MinKey,     NumberDouble, String,        Object,     Array,
                                     BinData,    Undefined,    jstOID,        Bool,       Date,
                                     jstNULL,    RegEx,        DBRef,         Code,       Symbol,
                                     CodeWScope, NumberInt,    bsonTimestamp, NumberLong, MaxKey};

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
    ASSERT_EQ(BSONType::NumberLong, bsonArrayLongLong[0].type());
    ASSERT_EQ(BSONType::NumberLong, bsonArrayLongLong[1].type());
    ASSERT_EQ(BSONType::NumberLong, bsonArrayLongLong[2].type());
    ASSERT_EQ(BSONType::NumberLong, bsonArrayLongLong[3].type());

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
