/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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
#include "mongo/db/query/vector_search/filter_validator.h"

#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/expression_internal_bucket_geo_within.h"
#include "mongo/db/matcher/expression_internal_eq_hashed_key.h"
#include "mongo/db/matcher/expression_internal_expr_comparison.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_text.h"
#include "mongo/db/matcher/expression_text_noop.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/expression_type.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/matcher/expression_where.h"
#include "mongo/db/matcher/expression_where_noop.h"
#include "mongo/db/matcher/match_expression_walker.h"
#include "mongo/db/matcher/schema/expression_internal_schema_all_elem_match_from_index.h"
#include "mongo/db/matcher/schema/expression_internal_schema_allowed_properties.h"
#include "mongo/db/matcher/schema/expression_internal_schema_cond.h"
#include "mongo/db/matcher/schema/expression_internal_schema_eq.h"
#include "mongo/db/matcher/schema/expression_internal_schema_fmod.h"
#include "mongo/db/matcher/schema/expression_internal_schema_match_array_index.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_length.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_properties.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_length.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_properties.h"
#include "mongo/db/matcher/schema/expression_internal_schema_num_array_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_object_match.h"
#include "mongo/db/matcher/schema/expression_internal_schema_root_doc_eq.h"
#include "mongo/db/matcher/schema/expression_internal_schema_str_length.h"
#include "mongo/db/matcher/schema/expression_internal_schema_unique_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_xor.h"

namespace mongo {

/**
 * A visitor to validate if the operators and types of the given $vectorSearch filter are all
 * supported. The visitor implements visit() methods for each MatchExpression subclass. With the
 * help from tree_walker::walk(), the visitor does not need to implement the traversal to the
 * children of a node.
 *
 * The supported operators and types are:
 * - Operators: $gte, $ge, $lte, $lt, $eq, $ne, $in, $nin, $and and $or
 * - Types: string, number and boolean.
 *
 * The visitor raises an assertion error when any unsupported expression is found during the
 * validation.
 */
class VectorSearchFilterValidator : public MatchExpressionConstVisitor {
public:
    void visit(const AlwaysFalseMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const AlwaysTrueMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    /**
     * Allows $and operator.
     */
    void visit(const AndMatchExpression* expr) override {}

    void visit(const BitsAllClearMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const BitsAllSetMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const BitsAnyClearMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const BitsAnySetMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const ElemMatchObjectMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const ElemMatchValueMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const EqualityMatchExpression* expr) override {}

    void visit(const ExistsMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const ExprMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const GTEMatchExpression* expr) override {}

    void visit(const GTMatchExpression* expr) override {}

    void visit(const GeoMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const GeoNearMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InMatchExpression* expr) override {
        uassert(7828302, str::stream() << "Matching null is not supported.", !expr->hasNull());
        uassert(7828303, str::stream() << "Matching regex is not supported.", !expr->hasRegex());
    }

    void visit(const InternalBucketGeoWithinMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalExprEqMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalExprGTMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalExprGTEMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalExprLTMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalExprLTEMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalEqHashedKey* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaAllElemMatchFromIndexMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaAllowedPropertiesMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaBinDataEncryptedTypeExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaBinDataFLE2EncryptedTypeExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaBinDataSubTypeExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaCondMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaEqMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaFmodMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaMatchArrayIndexMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaMaxItemsMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaMaxLengthMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaMaxPropertiesMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaMinItemsMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaMinLengthMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaMinPropertiesMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaObjectMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaRootDocEqMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaTypeExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaUniqueItemsMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const InternalSchemaXorMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const LTEMatchExpression* expr) override {}

    void visit(const LTMatchExpression* expr) override {}

    void visit(const ModMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const NorMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    /**
     * Allows $ne and $nin but disallows otherwise.
     */
    void visit(const NotMatchExpression* expr) override {
        MatchExpression::MatchType childType = expr->getChild(0)->matchType();
        // Only allows $ne and $nin.
        if (childType != MatchExpression::MatchType::EQ &&
            childType != MatchExpression::MatchType::MATCH_IN) {
            unsupportedExpression(expr);
        }
    }

    /**
     * Allows $or operator.
     */
    void visit(const OrMatchExpression* expr) override {}

    void visit(const RegexMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const SizeMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const TextMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const TextNoOpMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const TwoDPtInAnnulusExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const TypeMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const WhereMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

    void visit(const WhereNoOpMatchExpression* expr) override {
        unsupportedExpression(expr);
    }

private:
    void unsupportedExpression(const MatchExpression* expr) const {
        uasserted(7828300,
                  str::stream() << "Match expression is not supported for $vectorSearch: "
                                << expr->matchType());
    }
};

void validateVectorSearchFilter(const MatchExpression* expr) {
    VectorSearchFilterValidator visitor;
    // Performs pre-order traversal.
    MatchExpressionWalker walker(&visitor, nullptr /*inVisitor*/, nullptr /*postVisitor*/);
    tree_walker::walk<true, MatchExpression>(expr, &walker);
};

}  // namespace mongo
