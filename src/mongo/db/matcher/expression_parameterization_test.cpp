/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/matcher/expression_parameterization.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
void walkExpression(MatchExpressionParameterizationVisitorContext* context,
                    MatchExpression* expression) {
    MatchExpressionParameterizationVisitor visitor{context};
    MatchExpressionParameterizationWalker walker{&visitor};
    tree_walker::walk<false, MatchExpression>(expression, &walker);
}

struct CountInputParamsContext {
    size_t count{0};
};
class CountInputParamsVisitor : public MatchExpressionConstVisitor {
public:
    explicit CountInputParamsVisitor(CountInputParamsContext* context) : _context(context) {}

    void visit(const AlwaysFalseMatchExpression* expr) final {}
    void visit(const AlwaysTrueMatchExpression* expr) final {}
    void visit(const AndMatchExpression* expr) final {}
    void visit(const BitsAllClearMatchExpression* expr) final {
        return visitBitTestExpression(expr);
    }
    void visit(const BitsAllSetMatchExpression* expr) final {
        return visitBitTestExpression(expr);
    }
    void visit(const BitsAnyClearMatchExpression* expr) final {
        return visitBitTestExpression(expr);
    }
    void visit(const BitsAnySetMatchExpression* expr) final {
        return visitBitTestExpression(expr);
    }
    void visit(const ElemMatchObjectMatchExpression* matchExpr) final {}
    void visit(const ElemMatchValueMatchExpression* matchExpr) final {}
    void visit(const EqualityMatchExpression* expr) final {
        return visitComparisonMatchExpression(expr);
    }
    void visit(const ExistsMatchExpression* expr) final {}
    void visit(const ExprMatchExpression* expr) final {}
    void visit(const GTEMatchExpression* expr) final {
        return visitComparisonMatchExpression(expr);
    }
    void visit(const GTMatchExpression* expr) final {
        return visitComparisonMatchExpression(expr);
    }
    void visit(const GeoMatchExpression* expr) final {}
    void visit(const GeoNearMatchExpression* expr) final {}
    void visit(const InMatchExpression* expr) final {
        countParam(expr->getInputParamId());
    }
    void visit(const InternalBucketGeoWithinMatchExpression* expr) final {}
    void visit(const InternalExprEqMatchExpression* expr) final {}
    void visit(const InternalExprGTMatchExpression* expr) final {}
    void visit(const InternalExprGTEMatchExpression* expr) final {}
    void visit(const InternalExprLTMatchExpression* expr) final {}
    void visit(const InternalExprLTEMatchExpression* expr) final {}
    void visit(const InternalSchemaAllElemMatchFromIndexMatchExpression* expr) final {}
    void visit(const InternalSchemaAllowedPropertiesMatchExpression* expr) final {}
    void visit(const InternalSchemaBinDataEncryptedTypeExpression* expr) final {}
    void visit(const InternalSchemaBinDataFLE2EncryptedTypeExpression* expr) final {}
    void visit(const InternalSchemaBinDataSubTypeExpression* expr) final {}
    void visit(const InternalSchemaCondMatchExpression* expr) final {}
    void visit(const InternalSchemaEqMatchExpression* expr) final {}
    void visit(const InternalSchemaFmodMatchExpression* expr) final {}
    void visit(const InternalSchemaMatchArrayIndexMatchExpression* expr) final {}
    void visit(const InternalSchemaMaxItemsMatchExpression* expr) final {}
    void visit(const InternalSchemaMaxLengthMatchExpression* expr) final {}
    void visit(const InternalSchemaMaxPropertiesMatchExpression* expr) final {}
    void visit(const InternalSchemaMinItemsMatchExpression* expr) final {}
    void visit(const InternalSchemaMinLengthMatchExpression* expr) final {}
    void visit(const InternalSchemaMinPropertiesMatchExpression* expr) final {}
    void visit(const InternalSchemaObjectMatchExpression* expr) final {}
    void visit(const InternalSchemaRootDocEqMatchExpression* expr) final {}
    void visit(const InternalSchemaTypeExpression* expr) final {}
    void visit(const InternalSchemaUniqueItemsMatchExpression* expr) final {}
    void visit(const InternalSchemaXorMatchExpression* expr) final {}
    void visit(const LTEMatchExpression* expr) final {
        return visitComparisonMatchExpression(expr);
    }
    void visit(const LTMatchExpression* expr) final {
        return visitComparisonMatchExpression(expr);
    }
    void visit(const ModMatchExpression* expr) final {
        countParam(expr->getDivisorInputParamId());
        countParam(expr->getRemainder());
    }
    void visit(const NorMatchExpression* expr) final {}
    void visit(const NotMatchExpression* expr) final {}
    void visit(const OrMatchExpression* expr) final {}
    void visit(const RegexMatchExpression* expr) final {
        countParam(expr->getSourceRegexInputParamId());
        countParam(expr->getCompiledRegexInputParamId());
    }
    void visit(const SizeMatchExpression* expr) final {
        countParam(expr->getInputParamId());
    }
    void visit(const TextMatchExpression* expr) final {}
    void visit(const TextNoOpMatchExpression* expr) final {}
    void visit(const TwoDPtInAnnulusExpression* expr) final {}
    void visit(const TypeMatchExpression* expr) final {
        countParam(expr->getInputParamId());
    }
    void visit(const WhereMatchExpression* expr) final {
        countParam(expr->getInputParamId());
    }
    void visit(const WhereNoOpMatchExpression* expr) final {}

private:
    void visitComparisonMatchExpression(const ComparisonMatchExpressionBase* expr) {
        countParam(expr->getInputParamId());
    }

    void visitBitTestExpression(const BitTestMatchExpression* expr) {
        countParam(expr->getBitPositionsParamId());
        countParam(expr->getBitMaskParamId());
    }

    void countParam(boost::optional<MatchExpression::InputParamId> param) {
        if (param) {
            _context->count++;
        }
    }

    CountInputParamsContext* _context;
};

class CountInputParamWalker {
public:
    explicit CountInputParamWalker(CountInputParamsVisitor* visitor) : _visitor{visitor} {
        invariant(_visitor);
    }

    void preVisit(const MatchExpression* expr) {
        expr->acceptVisitor(_visitor);
    }

    void postVisit(const MatchExpression* expr) {}

    void inVisit(long count, const MatchExpression* expr) {}

private:
    CountInputParamsVisitor* _visitor;
};

/**
 * Return an number of parameters that are set within given expression tree.
 */
size_t countInputParams(const MatchExpression* expression) {
    CountInputParamsContext context;
    CountInputParamsVisitor visitor{&context};
    CountInputParamWalker walker{&visitor};
    tree_walker::walk<true, MatchExpression>(expression, &walker);
    return context.count;
}

}  // namespace

TEST(MatchExpressionParameterizationVisitor, AlwaysFalseMatchExpressionSetsNoParamIds) {
    AlwaysFalseMatchExpression expr{};
    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    expr.acceptVisitor(&visitor);
    ASSERT_EQ(0, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor, AlwaysTrueMatchExpressionSetsNoParamIds) {
    AlwaysTrueMatchExpression expr{};
    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    expr.acceptVisitor(&visitor);
    ASSERT_EQ(0, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor, BitsAllClearMatchExpressionSetsTwoParamIds) {
    std::vector<uint32_t> bitPositions;
    BitsAllClearMatchExpression expr{"a"_sd, bitPositions};
    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    expr.acceptVisitor(&visitor);
    ASSERT_EQ(2, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor, BitsAllSetMatchExpressionSetsTwoParamIds) {
    std::vector<uint32_t> bitPositions;
    BitsAllSetMatchExpression expr{"a"_sd, bitPositions};
    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    expr.acceptVisitor(&visitor);
    ASSERT_EQ(2, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor, BitsAnyClearMatchExpressionSetsTwoParamIds) {
    std::vector<uint32_t> bitPositions{0, 1, 8};
    BitsAnyClearMatchExpression expr{"a"_sd, bitPositions};
    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    expr.acceptVisitor(&visitor);
    ASSERT_EQ(2, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor, BitsAnySetMatchExpressionSetsTwoParamIds) {
    std::vector<uint32_t> bitPositions{0, 1, 8};
    BitsAnySetMatchExpression expr{"a"_sd, bitPositions};
    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    expr.acceptVisitor(&visitor);
    ASSERT_EQ(2, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor,
     EqualityMatchExpressionWithScalarParameterSetsOneParamId) {
    BSONObj query = BSON("a" << 5);
    EqualityMatchExpression eq("a"_sd, query["a"]);
    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    eq.acceptVisitor(&visitor);
    ASSERT_EQ(1, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor, EqualityMatchExpressionWithNullSetsNoParamIds) {
    BSONObj query = BSON("a" << BSONNULL);
    EqualityMatchExpression eq{"a"_sd, query["a"]};
    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    eq.acceptVisitor(&visitor);
    ASSERT_EQ(0, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor, EqualityMatchExpressionWithArraySetsNoParamIds) {
    BSONObj query = BSON("a" << BSON_ARRAY(1 << 2));
    EqualityMatchExpression eq{"a"_sd, query["a"]};
    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    eq.acceptVisitor(&visitor);
    ASSERT_EQ(0, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor, EqualityMatchExpressionWithMinKeySetsNoParamIds) {
    BSONObj query = BSON("a" << MINKEY);
    EqualityMatchExpression eq{"a"_sd, query["a"]};
    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    eq.acceptVisitor(&visitor);
    ASSERT_EQ(0, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor, EqualityMatchExpressionWithMaxKeySetsNoParamIds) {
    BSONObj query = BSON("a" << MAXKEY);
    EqualityMatchExpression eq{"a"_sd, query["a"]};
    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    eq.acceptVisitor(&visitor);
    ASSERT_EQ(0, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor, EqualityMatchExpressionWithUndefinedThrows) {
    BSONObj query = BSON("a" << BSONUndefined);
    ASSERT_THROWS((EqualityMatchExpression{"a"_sd, query["a"]}), DBException);
}

TEST(MatchExpressionParameterizationVisitor, GTEMatchExpressionWithScalarParameterSetsOneParamId) {
    BSONObj query = BSON("$gte" << 5);
    GTEMatchExpression expr{"a"_sd, query["$gte"]};
    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    expr.acceptVisitor(&visitor);
    ASSERT_EQ(1, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor, GTEMatchExpressionWithUndefinedThrows) {
    BSONObj query = BSON("a" << BSONUndefined);
    ASSERT_THROWS((EqualityMatchExpression{"a"_sd, query["a"]}), DBException);
}

TEST(MatchExpressionParameterizationVisitor, GTMatchExpressionWithScalarParameterSetsOneParamId) {
    BSONObj query = BSON("$gte" << 5);
    GTMatchExpression expr{"a"_sd, query["$gte"]};
    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    expr.acceptVisitor(&visitor);
    ASSERT_EQ(1, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor, LTEMatchExpressionWithScalarParameterSetsOneParamId) {
    BSONObj query = BSON("$lte" << 5);
    LTEMatchExpression expr("a"_sd, query["$lte"]);
    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    expr.acceptVisitor(&visitor);
    ASSERT_EQ(1, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor, LTMatchExpressionWithScalarParameterSetsOneParamId) {
    BSONObj query = BSON("$lt" << 5);
    LTMatchExpression expr{"a"_sd, query["$lt"]};
    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    expr.acceptVisitor(&visitor);
    ASSERT_EQ(1, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor, ComparisonMatchExpressionsWithNaNSetsNoParamIds) {
    std::vector<std::unique_ptr<MatchExpression>> expressions;

    BSONObj doubleNaN = BSON("$lt" << std::numeric_limits<double>::quiet_NaN());
    expressions.emplace_back(std::make_unique<LTMatchExpression>("a"_sd, doubleNaN["$lt"]));

    BSONObj decimalNegativeNaN = BSON("$gt" << Decimal128::kNegativeNaN);
    expressions.emplace_back(
        std::make_unique<GTMatchExpression>("b"_sd, decimalNegativeNaN["$gt"]));

    BSONObj decimalPositiveNaN = BSON("c" << Decimal128::kPositiveNaN);
    expressions.emplace_back(
        std::make_unique<EqualityMatchExpression>("c"_sd, decimalPositiveNaN["c"]));

    OrMatchExpression expr{std::move(expressions)};

    MatchExpressionParameterizationVisitorContext context{};
    walkExpression(&context, &expr);

    ASSERT_EQ(0, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor, InMatchExpressionWithScalarsSetsOneParamId) {
    BSONObj operand = BSON_ARRAY(1 << "r" << true << 1.1);
    InMatchExpression expr{"a"_sd};
    std::vector<BSONElement> equalities{operand[0], operand[1], operand[2], operand[3]};
    ASSERT_OK(expr.setEqualities(std::move(equalities)));

    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    expr.acceptVisitor(&visitor);
    ASSERT_EQ(1, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor, InMatchExpressionWithNullSetsNoParamIds) {
    BSONObj operand = BSON_ARRAY(1 << "r" << true << BSONNULL);
    InMatchExpression expr{"a"_sd};
    std::vector<BSONElement> equalities{operand[0], operand[1], operand[2], operand[3]};
    ASSERT_OK(expr.setEqualities(std::move(equalities)));

    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    expr.acceptVisitor(&visitor);
    ASSERT_EQ(0, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor, InMatchExpressionWithRegexSetsNoParamIds) {
    BSONObj query = BSON("a" << BSON("$in" << BSON_ARRAY(BSONRegEx("/^regex/i"))));

    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    MatchExpressionParameterizationVisitorContext context{};
    walkExpression(&context, result.getValue().get());
    ASSERT_EQ(0, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor, ModMatchExpressionSetsTwoParamIds) {
    ModMatchExpression expr{"a"_sd, 1, 2};

    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    expr.acceptVisitor(&visitor);
    ASSERT_EQ(2, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor, RegexMatchExpressionSetsTwoParamIds) {
    RegexMatchExpression expr{""_sd, "b", ""};

    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    expr.acceptVisitor(&visitor);
    ASSERT_EQ(2, context.inputParamIdToExpressionMap.size());
    ASSERT_EQ(2, context.nextInputParamId(nullptr));
}

TEST(MatchExpressionParameterizationVisitor, SizeMatchExpressionSetsOneParamId) {
    SizeMatchExpression expr{"a"_sd, 2};

    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    expr.acceptVisitor(&visitor);
    ASSERT_EQ(1, context.inputParamIdToExpressionMap.size());
    ASSERT_EQ(1, context.nextInputParamId(nullptr));
}

TEST(MatchExpressionParameterizationVisitor, TypeMatchExpressionWithStringSetsOneParamId) {
    TypeMatchExpression expr{"a"_sd, BSONType::String};

    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    expr.acceptVisitor(&visitor);
    // TODO SERVER-64776: fix the test case
    ASSERT_EQ(0, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor, TypeMatchExpressionWithArraySetsNoParamIds) {
    TypeMatchExpression expr{"a"_sd, BSONType::Array};

    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    expr.acceptVisitor(&visitor);
    ASSERT_EQ(0, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor, ExprMatchExpressionSetsNoParamsIds) {
    BSONObj query = BSON("$expr" << BSON("$gte" << BSON_ARRAY("$a"
                                                              << "$b")));

    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    MatchExpressionParameterizationVisitorContext context{};
    walkExpression(&context, result.getValue().get());
    ASSERT_EQ(0, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor,
     AutoParametrizationWalkerSetsCorrectNumberOfParamsIds) {
    BSONObj equalityExpr = BSON("x" << 1);
    BSONObj gtExpr = BSON("y" << BSON("$gt" << 2));
    BSONObj inExpr = BSON("$in" << BSON_ARRAY("a"
                                              << "b"
                                              << "c"));
    BSONObj regexExpr = BSON("m" << BSONRegEx("/^regex/i"));
    BSONObj sizeExpr = BSON("n" << BSON("$size" << 1));

    BSONObj query = BSON("$or" << BSON_ARRAY(equalityExpr
                                             << gtExpr << BSON("z" << inExpr)
                                             << BSON("$and" << BSON_ARRAY(regexExpr << sizeExpr))));

    {
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
        ASSERT_TRUE(result.isOK());
        auto expression = result.getValue().get();

        auto inputParamIdToExpressionMap = MatchExpression::parameterize(expression);
        ASSERT_EQ(6, inputParamIdToExpressionMap.size());
        ASSERT_EQ(6, countInputParams(expression));
    }

    {
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
        ASSERT_TRUE(result.isOK());
        auto expression = result.getValue().get();

        auto inputParamIdToExpressionMap = MatchExpression::parameterize(expression, 6);
        ASSERT_EQ(6, inputParamIdToExpressionMap.size());
        ASSERT_EQ(6, countInputParams(expression));
    }

    {
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
        ASSERT_TRUE(result.isOK());
        auto expression = result.getValue().get();

        auto inputParamIdToExpressionMap = MatchExpression::parameterize(expression, 5);
        ASSERT_EQ(0, inputParamIdToExpressionMap.size());
        ASSERT_EQ(0, countInputParams(expression));
    }
}
}  // namespace mongo
