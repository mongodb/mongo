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

#pragma once

#include <cstdint>

#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_type.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/matcher/expression_where.h"

namespace mongo {
/**
 * A context to track assigned input parameter IDs for auto-parameterization.
 */
struct MatchExpressionParameterizationVisitorContext {
    using InputParamId = MatchExpression::InputParamId;

    InputParamId nextInputParamId(const MatchExpression* expr) {
        inputParamIdToExpressionMap.push_back(expr);
        return inputParamIdToExpressionMap.size() - 1;
    }

    // Map to from assigned InputParamId to parameterised MatchExpression. Although it is called a
    // map, it can be safely represented as a vector because in this class we control that
    // inputParamId is an increasing sequence of integers starting from 0.
    std::vector<const MatchExpression*> inputParamIdToExpressionMap;
};

/**
 * An implementation of a MatchExpression visitor which assigns an optional input parameter ID to
 * each node which is eligible for auto-parameterization:
 *  - BitsAllClearMatchExpression
 *  - BitsAllSetMatchExpression
 *  - BitsAnyClearMatchExpression
 *  - BitsAnySetMatchExpression
 *  - Comparison expressions, unless compared against MinKey, MaxKey, null or NaN value or array
 *      - EqualityMatchExpression
 *      - GTEMatchExpression
 *      - GTMatchExpression
 *      - LTEMatchExpression
 *      - LTMatchExpression
 *  - InMatchExpression, unless it contains an array, null or regexp value.
 *  - ModMatchExpression (two parameter IDs for the divider and reminder)
 *  - RegexMatchExpression (two parameter IDs for the compiled regex and raw value)
 *  - SizeMatchExpression
 *  - TypeMatchExpression, unless type value is Array
 *  - WhereMatchExpression
 */
class MatchExpressionParameterizationVisitor final : public MatchExpressionMutableVisitor {
public:
    MatchExpressionParameterizationVisitor(MatchExpressionParameterizationVisitorContext* context)
        : _context{context} {
        invariant(_context);
    }

    void visit(AlwaysFalseMatchExpression* expr) final {}
    void visit(AlwaysTrueMatchExpression* expr) final {}
    void visit(AndMatchExpression* expr) final {}
    void visit(BitsAllClearMatchExpression* expr) final;
    void visit(BitsAllSetMatchExpression* expr) final;
    void visit(BitsAnyClearMatchExpression* expr) final;
    void visit(BitsAnySetMatchExpression* expr) final;
    void visit(EncryptedBetweenMatchExpression* expr) final {}
    void visit(ElemMatchObjectMatchExpression* matchExpr) final {}
    void visit(ElemMatchValueMatchExpression* matchExpr) final {}
    void visit(EqualityMatchExpression* expr) final;
    void visit(ExistsMatchExpression* expr) final {}
    void visit(ExprMatchExpression* expr) final {}
    void visit(GTEMatchExpression* expr) final;
    void visit(GTMatchExpression* expr) final;
    void visit(GeoMatchExpression* expr) final {}
    void visit(GeoNearMatchExpression* expr) final {}
    void visit(InMatchExpression* expr) final;
    void visit(InternalBucketGeoWithinMatchExpression* expr) final {}
    void visit(InternalExprEqMatchExpression* expr) final {}
    void visit(InternalExprGTMatchExpression* expr) final {}
    void visit(InternalExprGTEMatchExpression* expr) final {}
    void visit(InternalExprLTMatchExpression* expr) final {}
    void visit(InternalExprLTEMatchExpression* expr) final {}
    void visit(InternalSchemaAllElemMatchFromIndexMatchExpression* expr) final {}
    void visit(InternalSchemaAllowedPropertiesMatchExpression* expr) final {}
    void visit(InternalSchemaBinDataEncryptedTypeExpression* expr) final {}
    void visit(InternalSchemaBinDataFLE2EncryptedTypeExpression* expr) final {}
    void visit(InternalSchemaBinDataSubTypeExpression* expr) final {}
    void visit(InternalSchemaCondMatchExpression* expr) final {}
    void visit(InternalSchemaEqMatchExpression* expr) final {}
    void visit(InternalSchemaFmodMatchExpression* expr) final {}
    void visit(InternalSchemaMatchArrayIndexMatchExpression* expr) final {}
    void visit(InternalSchemaMaxItemsMatchExpression* expr) final {}
    void visit(InternalSchemaMaxLengthMatchExpression* expr) final {}
    void visit(InternalSchemaMaxPropertiesMatchExpression* expr) final {}
    void visit(InternalSchemaMinItemsMatchExpression* expr) final {}
    void visit(InternalSchemaMinLengthMatchExpression* expr) final {}
    void visit(InternalSchemaMinPropertiesMatchExpression* expr) final {}
    void visit(InternalSchemaObjectMatchExpression* expr) final {}
    void visit(InternalSchemaRootDocEqMatchExpression* expr) final {}
    void visit(InternalSchemaTypeExpression* expr) final {}
    void visit(InternalSchemaUniqueItemsMatchExpression* expr) final {}
    void visit(InternalSchemaXorMatchExpression* expr) final {}
    void visit(LTEMatchExpression* expr) final;
    void visit(LTMatchExpression* expr) final;
    void visit(ModMatchExpression* expr) final;
    void visit(NorMatchExpression* expr) final {}
    void visit(NotMatchExpression* expr) final {}
    void visit(OrMatchExpression* expr) final {}
    void visit(RegexMatchExpression* expr) final;
    void visit(SizeMatchExpression* expr) final;
    void visit(TextMatchExpression* expr) final {}
    void visit(TextNoOpMatchExpression* expr) final {}
    void visit(TwoDPtInAnnulusExpression* expr) final {}
    void visit(TypeMatchExpression* expr) final;
    void visit(WhereMatchExpression* expr) final;
    void visit(WhereNoOpMatchExpression* expr) final {}

private:
    void visitComparisonMatchExpression(ComparisonMatchExpressionBase* expr);

    void visitBitTestExpression(BitTestMatchExpression* expr);

    MatchExpressionParameterizationVisitorContext* _context;
};

/**
 * A match expression tree walker compatible with tree_walker::walk() to be used with
 * MatchExpressionParameterizationVisitor.
 */
class MatchExpressionParameterizationWalker {
public:
    MatchExpressionParameterizationWalker(MatchExpressionParameterizationVisitor* visitor)
        : _visitor{visitor} {
        invariant(_visitor);
    }

    void preVisit(MatchExpression* expr) {
        expr->acceptVisitor(_visitor);
    }

    void postVisit(MatchExpression* expr) {}

    void inVisit(long count, MatchExpression* expr) {}

private:
    MatchExpressionParameterizationVisitor* _visitor;
};

}  // namespace mongo
