/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/expression_internal_expr_comparison.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_path.h"
#include "mongo/db/matcher/expression_text.h"
#include "mongo/db/matcher/expression_text_noop.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/expression_type.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/matcher/schema/expression_internal_schema_all_elem_match_from_index.h"
#include "mongo/db/matcher/schema/expression_internal_schema_cond.h"
#include "mongo/db/matcher/schema/expression_internal_schema_eq.h"
#include "mongo/db/matcher/schema/expression_internal_schema_fmod.h"
#include "mongo/db/matcher/schema/expression_internal_schema_match_array_index.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_max_length.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_min_length.h"
#include "mongo/db/matcher/schema/expression_internal_schema_object_match.h"
#include "mongo/db/matcher/schema/expression_internal_schema_root_doc_eq.h"
#include "mongo/db/matcher/schema/expression_internal_schema_unique_items.h"

namespace mongo {

namespace exec::matcher {

class MatchExpressionEvaluator : public MatchExpressionConstVisitor {
public:
    MatchExpressionEvaluator(const MatchableDocument* doc, MatchDetails* details = nullptr)
        : _doc(doc), _details(details), _result(false) {}

    void visit(const AlwaysFalseMatchExpression* expr) override {
        _result = false;
    }
    void visit(const AlwaysTrueMatchExpression* expr) override {
        _result = true;
    }
    void visit(const AndMatchExpression* expr) override {
        for (auto&& child : expr->getChildren()) {
            child->acceptVisitor(this);
            if (!_result) {
                if (_details) {
                    _details->resetOutput();
                }
                return;
            }
        }
        _result = true;
    }
    void visit(const BitsAllClearMatchExpression* expr) override {
        visitPathExpression(expr);
    }
    void visit(const BitsAllSetMatchExpression* expr) override {
        visitPathExpression(expr);
    }
    void visit(const BitsAnyClearMatchExpression* expr) override {
        visitPathExpression(expr);
    }
    void visit(const BitsAnySetMatchExpression* expr) override {
        visitPathExpression(expr);
    }
    void visit(const ElemMatchObjectMatchExpression* expr) override {
        visitPathExpression(expr);
    }
    void visit(const ElemMatchValueMatchExpression* expr) override {
        visitPathExpression(expr);
    }
    void visit(const EqualityMatchExpression* expr) override {
        visitPathExpression(expr);
    }
    void visit(const ExistsMatchExpression* expr) override {
        visitPathExpression(expr);
    }
    void visit(const ExprMatchExpression* expr) override;
    void visit(const GTEMatchExpression* expr) override {
        visitPathExpression(expr);
    }
    void visit(const GTMatchExpression* expr) override {
        visitPathExpression(expr);
    }
    void visit(const GeoMatchExpression* expr) override {
        visitPathExpression(expr);
    }
    void visit(const GeoNearMatchExpression* expr) override {
        visitPathExpression(expr);
    }
    void visit(const InMatchExpression* expr) override {
        visitPathExpression(expr);
    }
    void visit(const InternalBucketGeoWithinMatchExpression* expr) override;
    void visit(const InternalExprEqMatchExpression* expr) override {
        visitPathExpression(expr);
    }
    void visit(const InternalExprGTMatchExpression* expr) override {
        visitPathExpression(expr);
    }
    void visit(const InternalExprGTEMatchExpression* expr) override {
        visitPathExpression(expr);
    }
    void visit(const InternalExprLTMatchExpression* expr) override {
        visitPathExpression(expr);
    }
    void visit(const InternalExprLTEMatchExpression* expr) override {
        visitPathExpression(expr);
    }
    void visit(const InternalEqHashedKey* expr) override;
    void visit(const InternalSchemaAllElemMatchFromIndexMatchExpression* expr) override {
        visitPathExpression(expr);
    }
    void visit(const InternalSchemaAllowedPropertiesMatchExpression* expr) override;
    void visit(const InternalSchemaBinDataEncryptedTypeExpression* expr) override {
        visitPathExpression(expr);
    }
    void visit(const InternalSchemaBinDataFLE2EncryptedTypeExpression* expr) override {
        visitPathExpression(expr);
    }
    void visit(const InternalSchemaBinDataSubTypeExpression* expr) override {
        visitPathExpression(expr);
    }
    void visit(const InternalSchemaCondMatchExpression* expr) override;
    void visit(const InternalSchemaEqMatchExpression* expr) override {
        visitPathExpression(expr);
    }
    void visit(const InternalSchemaFmodMatchExpression* expr) override {
        visitPathExpression(expr);
    }
    void visit(const InternalSchemaMatchArrayIndexMatchExpression* expr) override {
        visitPathExpression(expr);
    }
    void visit(const InternalSchemaMaxItemsMatchExpression* expr) override {
        visitPathExpression(expr);
    }
    void visit(const InternalSchemaMaxLengthMatchExpression* expr) override {
        visitPathExpression(expr);
    }
    void visit(const InternalSchemaMaxPropertiesMatchExpression* expr) override;
    void visit(const InternalSchemaMinItemsMatchExpression* expr) override {
        visitPathExpression(expr);
    }
    void visit(const InternalSchemaMinLengthMatchExpression* expr) override {
        visitPathExpression(expr);
    }
    void visit(const InternalSchemaMinPropertiesMatchExpression* expr) override;
    void visit(const InternalSchemaObjectMatchExpression* expr) override {
        visitPathExpression(expr);
    }
    void visit(const InternalSchemaRootDocEqMatchExpression* expr) override {
        _result = expr->getObjCmp()->evaluate(_doc->toBSON() == expr->getRhsObj());
    }
    void visit(const InternalSchemaTypeExpression* expr) override {
        visitPathExpression(expr);
    }
    void visit(const InternalSchemaUniqueItemsMatchExpression* expr) override {
        visitPathExpression(expr);
    }
    void visit(const InternalSchemaXorMatchExpression* expr) override;
    void visit(const LTEMatchExpression* expr) override {
        visitPathExpression(expr);
    }
    void visit(const LTMatchExpression* expr) override {
        visitPathExpression(expr);
    }
    void visit(const ModMatchExpression* expr) override {
        visitPathExpression(expr);
    }
    void visit(const NorMatchExpression* expr) override {
        MatchExpressionEvaluator childVisitor(_doc, nullptr);
        for (auto&& child : expr->getChildren()) {
            child->acceptVisitor(&childVisitor);
            if (childVisitor.getResult()) {
                _result = false;
                return;
            }
        }
        _result = true;
    }
    void visit(const NotMatchExpression* expr) override;
    void visit(const OrMatchExpression* expr) override {
        MatchExpressionEvaluator childVisitor(_doc, nullptr);
        for (auto&& child : expr->getChildren()) {
            child->acceptVisitor(&childVisitor);
            if (childVisitor.getResult()) {
                _result = true;
                return;
            }
        }
        _result = false;
    }
    void visit(const RegexMatchExpression* expr) override {
        visitPathExpression(expr);
    }
    void visit(const SizeMatchExpression* expr) override {
        visitPathExpression(expr);
    }
    void visit(const TextMatchExpression* expr) override {
        visitPathExpression(expr);
    }
    void visit(const TextNoOpMatchExpression* expr) override {
        visitPathExpression(expr);
    }
    void visit(const TwoDPtInAnnulusExpression* expr) override {
        visitPathExpression(expr);
    }
    void visit(const TypeMatchExpression* expr) override {
        visitPathExpression(expr);
    }
    void visit(const WhereMatchExpression* expr) override;
    void visit(const WhereNoOpMatchExpression* expr) override;

    bool getResult() const {
        return _result;
    }

private:
    void visitPathExpression(const PathMatchExpression* expr);

    const MatchableDocument* _doc;
    MatchDetails* _details;
    bool _result;
};

//
// Determine if a document satisfies the tree-predicate.
//
inline bool matches(const MatchExpression* expr,
                    const MatchableDocument* doc,
                    MatchDetails* details = nullptr) {
    MatchExpressionEvaluator evaluator(doc, details);
    expr->acceptVisitor(&evaluator);
    return evaluator.getResult();
}

inline bool matchesBSON(const MatchExpression* expr,
                        const BSONObj& doc,
                        MatchDetails* details = nullptr) {
    BSONMatchableDocument mydoc(doc);
    return matches(expr, &mydoc, details);
}

/**
 * Determines if 'elem' would satisfy the predicate if wrapped with the top-level field name of
 * the predicate. Does not check that the predicate has a single top-level field name. For
 * example, given the object obj={a: [5]}, the predicate {i: {$gt: 0}} would match the element
 * obj["a"]["0"] because it performs the match as if the element at "a.0" were the BSONObj {i:
 * 5}.
 */
inline bool matchesBSONElement(const MatchExpression* expr,
                               BSONElement elem,
                               MatchDetails* details = nullptr) {
    BSONElementViewMatchableDocument matchableDoc(elem);
    return matches(expr, &matchableDoc, details);
}

/**
 * Evaluates the Expression stored in a ExprMatchExpression using the proper configuration.
 */
Value evaluateExpression(const ExprMatchExpression* expr, const MatchableDocument* doc);

}  // namespace exec::matcher
}  // namespace mongo
