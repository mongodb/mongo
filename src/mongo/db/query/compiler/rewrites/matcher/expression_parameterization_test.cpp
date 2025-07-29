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

#include "mongo/db/query/compiler/rewrites/matcher/expression_parameterization.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/matcher_type_set.h"
#include "mongo/db/matcher/schema/expression_internal_schema_allowed_properties.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/tree_walker.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {
void walkExpression(MatchExpressionParameterizationVisitorContext* context,
                    MatchExpression* expression) {
    MatchExpressionParameterizationVisitor visitor{context};
    MatchExpressionParameterizationWalker walker{&visitor};
    tree_walker::walk<false, MatchExpression>(expression, &walker);
}

struct CollectInputParamsContext {
    std::vector<size_t> paramIds;
};
class CollectInputParamsVisitor : public MatchExpressionConstVisitor {
public:
    explicit CollectInputParamsVisitor(CollectInputParamsContext* context) : _context(context) {}

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
    void visit(const InternalEqHashedKey*) final {}
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
        countParam(expr->getRemainderInputParamId());
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
            _context->paramIds.push_back(*param);
        }
    }

    CollectInputParamsContext* _context;
};

class CollectInputParamWalker {
public:
    explicit CollectInputParamWalker(CollectInputParamsVisitor* visitor) : _visitor{visitor} {
        invariant(_visitor);
    }

    void preVisit(const MatchExpression* expr) {
        expr->acceptVisitor(_visitor);
    }

    void postVisit(const MatchExpression* expr) {}

    void inVisit(long count, const MatchExpression* expr) {}

private:
    CollectInputParamsVisitor* _visitor;
};

/**
 * Return the list of parameter numbers that are set within given expression tree.
 */
std::vector<size_t> collectInputParams(const MatchExpression* expression) {
    CollectInputParamsContext context;
    CollectInputParamsVisitor visitor{&context};
    CollectInputParamWalker walker{&visitor};
    tree_walker::walk<true, MatchExpression>(expression, &walker);
    return context.paramIds;
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

TEST(MatchExpressionParameterizationVisitor, ComparisonMatchExpressionsWithBooleanSetsNoParamIds) {
    std::vector<std::unique_ptr<MatchExpression>> expressions;
    expressions.emplace_back(std::make_unique<LTMatchExpression>("a"_sd, Value(false)));
    expressions.emplace_back(std::make_unique<GTMatchExpression>("b"_sd, Value(true)));
    expressions.emplace_back(std::make_unique<EqualityMatchExpression>("c"_sd, Value(false)));
    expressions.emplace_back(std::make_unique<LTEMatchExpression>("d"_sd, Value(true)));
    expressions.emplace_back(std::make_unique<GTEMatchExpression>("e"_sd, Value(false)));

    OrMatchExpression expr{std::move(expressions)};

    MatchExpressionParameterizationVisitorContext context{};
    walkExpression(&context, &expr);

    ASSERT_EQ(0, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor,
     ComparisonMatchExpressionsWithEmptyStringSetsNoParamIds) {
    std::vector<std::unique_ptr<MatchExpression>> expressions;
    expressions.emplace_back(std::make_unique<LTMatchExpression>("a"_sd, Value(""_sd)));
    expressions.emplace_back(std::make_unique<GTMatchExpression>("b"_sd, Value(""_sd)));
    expressions.emplace_back(std::make_unique<EqualityMatchExpression>("c"_sd, Value(""_sd)));
    expressions.emplace_back(std::make_unique<LTEMatchExpression>("d"_sd, Value(""_sd)));
    expressions.emplace_back(std::make_unique<GTEMatchExpression>("e"_sd, Value(""_sd)));

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

TEST(MatchExpressionParameterizationVisitor, InMatchExpressionWithScalarsReusesOneParamId) {
    BSONObj operand = BSON_ARRAY(1 << "r" << true << 1.1);
    InMatchExpression expr{"a"_sd};
    std::vector<BSONElement> equalities{operand[0], operand[1], operand[2], operand[3]};
    ASSERT_OK(expr.setEqualities(std::move(equalities)));

    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    expr.acceptVisitor(&visitor);
    ASSERT_EQ(1, context.inputParamIdToExpressionMap.size());

    InMatchExpression expr2{"a"_sd};
    std::vector<BSONElement> equalities2{operand[0], operand[1], operand[2], operand[3]};
    ASSERT_OK(expr.setEqualities(std::move(equalities2)));
    expr.acceptVisitor(&visitor);

    // The second $in reused the previouse parameterId.
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

    auto expCtx = make_intrusive<ExpressionContextForTest>();
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
    TypeMatchExpression expr{"a"_sd, BSONType::string};

    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    expr.acceptVisitor(&visitor);
    // TODO SERVER-64776: fix the test case
    ASSERT_EQ(0, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor, TypeMatchExpressionWithArraySetsNoParamIds) {
    TypeMatchExpression expr{"a"_sd, BSONType::array};

    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    expr.acceptVisitor(&visitor);
    ASSERT_EQ(0, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor, ExprMatchExpressionSetsNoParamsIds) {
    BSONObj query = BSON("$expr" << BSON("$gte" << BSON_ARRAY("$a" << "$b")));

    auto expCtx = make_intrusive<ExpressionContextForTest>();
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    MatchExpressionParameterizationVisitorContext context{};
    walkExpression(&context, result.getValue().get());
    ASSERT_EQ(0, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor, OrMatchExpressionSetsOneParam) {
    BSONObj query = BSON("$or" << BSON_ARRAY(BSON("a" << 1) << BSON("a" << 1)));

    auto expCtx = make_intrusive<ExpressionContextForTest>();
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    MatchExpressionParameterizationVisitorContext context{};
    walkExpression(&context, result.getValue().get());
    ASSERT_EQ(1, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor, OrMatchExpressionSetsThreeParamWithFourPredicates) {
    BSONObj query =
        BSON("$or" << BSON_ARRAY(BSON("a" << 1) << BSON("b" << 2)) << "a" << 1 << "b" << 3);

    auto expCtx = make_intrusive<ExpressionContextForTest>();
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    MatchExpressionParameterizationVisitorContext context{};
    walkExpression(&context, result.getValue().get());
    ASSERT_EQ(3, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor,
     MapIsUsedForParamIdLookupsAfterContextSizeExceedsThreshold) {
    MatchExpressionParameterizationVisitorContext context{};
    // Ensure lifetime of BSONObjs matches that of MatchExpressions.
    std::vector<BSONObj> expressionBSONs;
    std::vector<std::unique_ptr<MatchExpression>> expressions;

    auto expectedSize = context.kUseMapThreshold;

    auto addExpressions = [&]() {
        for (size_t i = 0; i < expectedSize; i++) {
            BSONObj gt = BSON("$gt" << static_cast<int>(i));
            expressionBSONs.push_back(gt);
            expressions.emplace_back(std::make_unique<GTMatchExpression>("a"_sd, gt["$gt"]));
        }
    };

    // Add the same set of 50 expressions twice. For duplicate expressions, param ids will be reused
    // by doing a lookup on the map.
    addExpressions();
    addExpressions();

    OrMatchExpression expr{std::move(expressions)};
    walkExpression(&context, &expr);

    // Ensure param ids were reused for duplicate expressions.
    ASSERT_EQ(expectedSize, context.inputParamIdToExpressionMap.size());
    // The tasserts in MatchExpressionParameterizationVisitorContext::nextReusableInputParamId
    // ensure the map is actually used for lookups once the amount of input param ids exceeds
    // kUseMapThreshold.
    ASSERT_TRUE(context.inputParamIdToExpressionMap.usingMap());
}

TEST(MatchExpressionParameterizationVisitor,
     MapIsNotUsedForParamIdLookupsUntilContextSizeExceedsThreshold) {
    MatchExpressionParameterizationVisitorContext context{};
    // Ensure lifetime of BSONObjs matches that of MatchExpressions.
    std::vector<BSONObj> expressionBSONs;
    std::vector<std::unique_ptr<MatchExpression>> expressions;

    auto expectedSize = context.kUseMapThreshold - 1;

    auto addExpressions = [&]() {
        for (size_t i = 0; i < expectedSize; i++) {
            BSONObj gt = BSON("$gt" << static_cast<int>(i));
            expressionBSONs.push_back(gt);
            expressions.emplace_back(std::make_unique<GTMatchExpression>("a"_sd, gt["$gt"]));
        }
    };

    // Add the same set of 49 expressions twice. For duplicate expressions, param ids will be reused
    // by doing a lookup on the vector.
    addExpressions();
    addExpressions();

    OrMatchExpression expr{std::move(expressions)};
    walkExpression(&context, &expr);

    // Ensure param ids were reused for duplicate expressions.
    ASSERT_EQ(expectedSize, context.inputParamIdToExpressionMap.size());
    ASSERT_FALSE(context.inputParamIdToExpressionMap.usingMap());
}

TEST(MatchExpressionParameterizationVisitor,
     AutoParametrizationWalkerSetsCorrectNumberOfParamsIds) {
    BSONObj equalityExpr = BSON("x" << 1);
    BSONObj gtExpr = BSON("y" << BSON("$gt" << 2));
    BSONObj inExpr = BSON("$in" << BSON_ARRAY("a" << "b"
                                                  << "c"));
    BSONObj regexExpr = BSON("m" << BSONRegEx("/^regex/i"));
    BSONObj sizeExpr = BSON("n" << BSON("$size" << 1));

    BSONObj query = BSON("$or" << BSON_ARRAY(equalityExpr
                                             << gtExpr << BSON("z" << inExpr)
                                             << BSON("$and" << BSON_ARRAY(regexExpr << sizeExpr))));

    {
        auto expCtx = make_intrusive<ExpressionContextForTest>();
        StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
        ASSERT_TRUE(result.isOK());
        MatchExpression* expression = result.getValue().get();

        auto inputParamIdToExpressionMap = parameterizeMatchExpression(expression);
        ASSERT_EQ(6, inputParamIdToExpressionMap.size());
        ASSERT_EQ(6, collectInputParams(expression).size());
    }

    {
        auto expCtx = make_intrusive<ExpressionContextForTest>();
        StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
        ASSERT_TRUE(result.isOK());
        MatchExpression* expression = result.getValue().get();

        auto inputParamIdToExpressionMap = parameterizeMatchExpression(expression, 6);
        ASSERT_EQ(6, inputParamIdToExpressionMap.size());
        ASSERT_EQ(6, collectInputParams(expression).size());
    }

    {
        auto expCtx = make_intrusive<ExpressionContextForTest>();
        StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
        ASSERT_TRUE(result.isOK());
        MatchExpression* expression = result.getValue().get();

        // This tests the optional limit on the maximum number of parameters allowed.
        auto inputParamIdToExpressionMap = parameterizeMatchExpression(expression, 5);
        ASSERT_EQ(5, inputParamIdToExpressionMap.size());
        ASSERT_EQ(5, collectInputParams(expression).size());
    }

    {
        // Test that the optional number of parameters limit is all-or-nothing for a binary op.
        BSONObj query2 = BSON("a" << BSON("$mod" << BSON_ARRAY(1 << 2)) << "b"
                                  << BSON("$mod" << BSON_ARRAY(3 << 4)));
        auto expCtx = make_intrusive<ExpressionContextForTest>();
        StatusWithMatchExpression result = MatchExpressionParser::parse(query2, expCtx);
        ASSERT_TRUE(result.isOK());
        MatchExpression* expression = result.getValue().get();

        // Both operands of a binary op must be parameterized or not parameterized. There are two
        // binary ops for a total of four possible parameters, each of which has a unique value so
        // cannot share a prior parameter. With a limit of 3, the first two get parameterized but
        // the last two do not.
        auto inputParamIdToExpressionMap = parameterizeMatchExpression(expression, 3);
        ASSERT_EQ(2, inputParamIdToExpressionMap.size());
        ASSERT_EQ(2, collectInputParams(expression).size());
    }
}

TEST(MatchExpressionParameterizationVisitor, AutoParametrizationWalkerSetsCorrectReusedParamsIds) {
    BSONObj repeatedEqualityExpr = BSON("x" << 1);

    BSONObj query = BSON("$or" << BSON_ARRAY(repeatedEqualityExpr << repeatedEqualityExpr));

    {
        auto expCtx = make_intrusive<ExpressionContextForTest>();
        StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
        ASSERT_TRUE(result.isOK());
        MatchExpression* expression = result.getValue().get();

        auto inputParamIdToExpressionMap = parameterizeMatchExpression(expression);
        // Check that the two MatchExpression reuse the same parameter.
        ASSERT_EQ(1, inputParamIdToExpressionMap.size());
        auto parameters = collectInputParams(expression);
        ASSERT_EQ(2, parameters.size());
        ASSERT_EQ(parameters[0], parameters[1]);
    }

    {
        auto expCtx = make_intrusive<ExpressionContextForTest>();
        StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
        ASSERT_TRUE(result.isOK());
        MatchExpression* expression = result.getValue().get();

        auto inputParamIdToExpressionMap = parameterizeMatchExpression(expression, boost::none, 15);
        ASSERT_EQ(1, inputParamIdToExpressionMap.size());
        auto parameters = collectInputParams(expression);
        ASSERT_EQ(2, parameters.size());
        ASSERT_EQ(parameters[0], parameters[1]);
        // Check that the number of the parameter is indeed starting with parameter ID that was
        // specified in the call to parameterize().
        ASSERT_EQ(parameters[0], 15);
    }

    {
        // Create a query where the reused expression is found a first time while the internal
        // structure has not yet mutated to use a map, and another time when it is already using a
        // map.
        // Query will look like:
        //
        //  $and: [
        //     {$or: [{x: 1}, {x: 1}]},
        //     {y: 0},
        //     {y: 1},
        //     ....
        //     {y: 25},
        //     {$or: [{x: 1}, {x: 1}]},
        //     {y: 26},
        //     ....
        //     {y: 49},
        //     {$or: [{x: 1}, {x: 1}]}
        //  ]
        BSONArrayBuilder builder;
        builder << query;
        for (size_t i = 0; i < MatchExpressionParameterizationVisitorContext::kUseMapThreshold;
             i++) {
            builder << BSON("y" << static_cast<int>(i));
            if (i == MatchExpressionParameterizationVisitorContext::kUseMapThreshold / 2) {
                builder << query;
            }
        }
        builder << query;
        BSONObj longQuery = BSON("$and" << builder.arr());

        auto expCtx = make_intrusive<ExpressionContextForTest>();
        StatusWithMatchExpression result = MatchExpressionParser::parse(longQuery, expCtx);
        ASSERT_TRUE(result.isOK());
        MatchExpression* expression = result.getValue().get();

        auto inputParamIdToExpressionMap = parameterizeMatchExpression(expression, boost::none, 15);
        // There should be kUseMapThreshold expressions plus 1.
        ASSERT_EQ(MatchExpressionParameterizationVisitorContext::kUseMapThreshold + 1,
                  inputParamIdToExpressionMap.size());
        auto parameters = collectInputParams(expression);
        // There should be kUseMapThreshold plus 3 x 2 references to input parameters.
        ASSERT_EQ(MatchExpressionParameterizationVisitorContext::kUseMapThreshold + 6,
                  parameters.size());
        // The first two expressions and the last two should be the reused paramters.
        ASSERT_EQ(parameters[0], parameters[1]);
        ASSERT_EQ(parameters[0], parameters.back());
        ASSERT_EQ(parameters[0], 15);
        // Check that no parameter ID is lower than the one specified in the call to
        // parameterize().
        auto minIt = std::min_element(parameters.begin(), parameters.end());
        ASSERT_EQ(*minIt, 15);
        // Check that the span between lowest and highest parameter ID is equal to the number of
        // unique parameters.
        auto maxIt = std::max_element(parameters.begin(), parameters.end());
        ASSERT_EQ(*maxIt, 15 + inputParamIdToExpressionMap.size() - 1);
    }
}

}  // namespace mongo
