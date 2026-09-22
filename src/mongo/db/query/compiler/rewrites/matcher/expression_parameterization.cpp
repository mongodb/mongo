// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/rewrites/matcher/expression_parameterization.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/assert_util.h"

#include <cmath>

namespace mongo {

void MatchExpressionParameterizationVisitor::visitBitTestExpression(BitTestMatchExpression* expr) {
    if (_context->availableParamIds(2)) {
        expr->setBitPositionsParamId(_context->nextInputParamId(expr));
        expr->setBitMaskParamId(_context->nextInputParamId(expr));
    }
}

void MatchExpressionParameterizationVisitor::visit(BitsAllClearMatchExpression* expr) {
    visitBitTestExpression(expr);
}

void MatchExpressionParameterizationVisitor::visit(BitsAllSetMatchExpression* expr) {
    visitBitTestExpression(expr);
}

void MatchExpressionParameterizationVisitor::visit(BitsAnyClearMatchExpression* expr) {
    visitBitTestExpression(expr);
}

void MatchExpressionParameterizationVisitor::visit(BitsAnySetMatchExpression* expr) {
    visitBitTestExpression(expr);
}

void MatchExpressionParameterizationVisitor::visit(EqualityMatchExpression* expr) {
    visitComparisonMatchExpression(expr);
}

void MatchExpressionParameterizationVisitor::visit(GTEMatchExpression* expr) {
    visitComparisonMatchExpression(expr);
}

void MatchExpressionParameterizationVisitor::visit(GTMatchExpression* expr) {
    visitComparisonMatchExpression(expr);
}

void MatchExpressionParameterizationVisitor::visit(LTEMatchExpression* expr) {
    visitComparisonMatchExpression(expr);
}

void MatchExpressionParameterizationVisitor::visit(LTMatchExpression* expr) {
    visitComparisonMatchExpression(expr);
}

void MatchExpressionParameterizationVisitor::visit(ModMatchExpression* expr) {
    if (_context->availableParamIds(2)) {
        expr->setDivisorInputParamId(_context->nextInputParamId(expr));
        expr->setRemainderInputParamId(_context->nextInputParamId(expr));
    }
}

void MatchExpressionParameterizationVisitor::visit(RegexMatchExpression* expr) {
    if (_context->availableParamIds(2)) {
        expr->setSourceRegexInputParamId(_context->nextInputParamId(expr));
        expr->setCompiledRegexInputParamId(_context->nextInputParamId(expr));
    }
}

void MatchExpressionParameterizationVisitor::visit(SizeMatchExpression* expr) {
    expr->setInputParamId(_context->nextInputParamId(expr));
}

void MatchExpressionParameterizationVisitor::visit(WhereMatchExpression* expr) {
    expr->setInputParamId(_context->nextInputParamId(expr));
}

bool isEligibleForComparisonAutoParameterization(const BSONElement& data) {
    switch (data.type()) {
        case BSONType::minKey:
        case BSONType::eoo:
        case BSONType::null:
        case BSONType::array:
        case BSONType::dbRef:
        case BSONType::maxKey:
        case BSONType::undefined:
        case BSONType::object:
        case BSONType::boolean:
            return false;

        case BSONType::string:
            return !data.str().empty();
        case BSONType::binData:
        case BSONType::oid:
        case BSONType::regEx:
        case BSONType::code:
        case BSONType::symbol:
        case BSONType::codeWScope:
            return true;
        case BSONType::timestamp:
            return data.timestamp() != Timestamp::max() && data.timestamp() != Timestamp::min();
        case BSONType::date:
            return data.Date() != Date_t::max() && data.Date() != Date_t::min();
        case BSONType::numberInt:
            return data.numberInt() != std::numeric_limits<int>::max() &&
                data.numberInt() != std::numeric_limits<int>::min();
        case BSONType::numberLong:
            return data.numberLong() != std::numeric_limits<long long>::max() &&
                data.numberLong() != std::numeric_limits<long long>::min();
        case BSONType::numberDouble: {
            auto doubleVal = data.numberDouble();
            return !std::isnan(doubleVal) && doubleVal != std::numeric_limits<double>::max() &&
                doubleVal != std::numeric_limits<double>::min() &&
                doubleVal != std::numeric_limits<double>::infinity() &&
                doubleVal != -std::numeric_limits<double>::infinity();
        }
        case BSONType::numberDecimal:
            return !data.numberDecimal().isNaN() && !data.numberDecimal().isInfinite();
    }
    MONGO_UNREACHABLE_TASSERT(13408005);
}

void MatchExpressionParameterizationVisitor::visitComparisonMatchExpression(
    ComparisonMatchExpressionBase* expr) {
    if (isEligibleForComparisonAutoParameterization(expr->getData())) {
        expr->setInputParamId(_context->nextReusableInputParamId(expr));
    }
}

void MatchExpressionParameterizationVisitor::visit(InMatchExpression* expr) {
    // We don't set inputParamId if a InMatchExpression contains a regex.
    if (!expr->getRegexes().empty()) {
        return;
    }

    // We don't set inputParamId if there's just one element because it could end up with a single
    // interval index bound that may be eligible for fast COUNT_SCAN plan. However, a
    // multiple-element $in query has more than one (point) intervals for the index bounds, which is
    // ineligible for COUNT_SCAN. This is to make sure that $in queries with multiple elements will
    // not share the same query shape with any other single-element $in query.
    if (expr->equalitiesHasSingleElement()) {
        return;
    }

    if (expr->hasNull() || expr->hasArray() || expr->hasObject()) {
        // We don't set inputParamId if an InMatchExpression contains null, arrays, or objects.
        return;
    }

    expr->setInputParamId(_context->nextReusableInputParamId(expr));
}

void MatchExpressionParameterizationVisitor::visit(TypeMatchExpression* expr) {
    // TODO SERVER-64776: reenable auto-parameterization for $type expressions.
}

std::vector<const MatchExpression*> parameterizeMatchExpression(
    MatchExpression* tree,
    boost::optional<size_t> maxParamCount,
    MatchExpression::InputParamId startingParamId,
    bool* parameterized) {
    MatchExpressionParameterizationVisitorContext context{maxParamCount, startingParamId};
    MatchExpressionParameterizationVisitor visitor{&context};
    MatchExpressionParameterizationWalker walker{&visitor};
    tree_walker::walk<false, MatchExpression>(tree, &walker);

    // If the caller provided a non-null 'parameterized' argument, set this output.
    if (parameterized != nullptr) {
        *parameterized = context.parameterized;
    }

    return std::move(context.inputParamIdToExpressionMap);
}

std::vector<const MatchExpression*> unparameterizeMatchExpression(MatchExpression* tree) {
    return parameterizeMatchExpression(tree, 0);
}

}  // namespace mongo
