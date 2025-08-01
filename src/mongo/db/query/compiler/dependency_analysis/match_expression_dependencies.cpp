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

#include "mongo/db/query/compiler/dependency_analysis/match_expression_dependencies.h"

#include "mongo/base/string_data.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_expr.h"
#include "mongo/db/matcher/expression_geo.h"
#include "mongo/db/matcher/expression_internal_bucket_geo_within.h"
#include "mongo/db/matcher/expression_internal_eq_hashed_key.h"
#include "mongo/db/matcher/expression_internal_expr_comparison.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_path.h"
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
#include "mongo/db/matcher/schema/expression_internal_schema_object_match.h"
#include "mongo/db/matcher/schema/expression_internal_schema_root_doc_eq.h"
#include "mongo/db/matcher/schema/expression_internal_schema_unique_items.h"
#include "mongo/db/matcher/schema/expression_internal_schema_xor.h"
#include "mongo/db/query/compiler/dependency_analysis/expression_dependencies.h"
#include "mongo/db/query/tree_walker.h"

#include <cstddef>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::dependency_analysis {

namespace {

class PostVisitor : public SelectiveMatchExpressionVisitorBase<true> {
public:
    using SelectiveMatchExpressionVisitorBase<true>::visit;

    PostVisitor(bool& ignoreDependencies) : _ignoreDependencies(ignoreDependencies) {}

    void visit(const ElemMatchObjectMatchExpression* expr) override {
        _ignoreDependencies = false;
    }

    void visit(const ElemMatchValueMatchExpression* expr) override {
        _ignoreDependencies = false;
    }

    void visit(const InternalSchemaObjectMatchExpression* expr) override {
        _ignoreDependencies = false;
    }

private:
    bool& _ignoreDependencies;
};

class DependencyVisitor : public MatchExpressionConstVisitor {
public:
    DependencyVisitor(DepsTracker* deps, bool& ignoreDependencies)
        : _deps(deps), _ignoreDependencies(ignoreDependencies) {}

    void visit(const AlwaysFalseMatchExpression* expr) override {}

    void visit(const AlwaysTrueMatchExpression* expr) override {}

    void visit(const AndMatchExpression* expr) override {}

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
        _ignoreDependencies = true;
    }

    void visit(const ElemMatchValueMatchExpression* expr) override {
        visitPathExpression(expr);
        _ignoreDependencies = true;
    }

    void visit(const EqualityMatchExpression* expr) override {
        visitPathExpression(expr);
    }

    void visit(const ExistsMatchExpression* expr) override {
        visitPathExpression(expr);
    }

    void visit(const ExprMatchExpression* expr) override {
        if (_ignoreDependencies) {
            return;
        }

        if (expr->getExpression()) {
            mongo::expression::addDependencies(expr->getExpression().get(), _deps);
        }
    }

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

    void visit(const InternalBucketGeoWithinMatchExpression* expr) override {
        if (_ignoreDependencies) {
            return;
        }
        _deps->needWholeDocument = true;
    }

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

    void visit(const InternalEqHashedKey* expr) override {
        visitPathExpression(expr);
    }

    void visit(const InternalSchemaAllElemMatchFromIndexMatchExpression* expr) override {
        visitPathExpression(expr);
    }

    void visit(const InternalSchemaAllowedPropertiesMatchExpression* expr) override {
        if (_ignoreDependencies) {
            return;
        }
        _deps->needWholeDocument = true;
    }

    void visit(const InternalSchemaBinDataEncryptedTypeExpression* expr) override {
        visitPathExpression(expr);
    }

    void visit(const InternalSchemaBinDataFLE2EncryptedTypeExpression* expr) override {
        visitPathExpression(expr);
    }

    void visit(const InternalSchemaBinDataSubTypeExpression* expr) override {
        visitPathExpression(expr);
    }

    void visit(const InternalSchemaCondMatchExpression* expr) override {}

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

    void visit(const InternalSchemaMaxPropertiesMatchExpression* expr) override {
        if (_ignoreDependencies) {
            return;
        }
        _deps->needWholeDocument = true;
    }

    void visit(const InternalSchemaMinItemsMatchExpression* expr) override {
        visitPathExpression(expr);
    }

    void visit(const InternalSchemaMinLengthMatchExpression* expr) override {
        visitPathExpression(expr);
    }

    void visit(const InternalSchemaMinPropertiesMatchExpression* expr) override {
        if (_ignoreDependencies) {
            return;
        }
        _deps->needWholeDocument = true;
    }

    void visit(const InternalSchemaObjectMatchExpression* expr) override {
        visitPathExpression(expr);
        _ignoreDependencies = true;
    }

    void visit(const InternalSchemaRootDocEqMatchExpression* expr) override {
        if (_ignoreDependencies) {
            return;
        }
        _deps->needWholeDocument = true;
    }

    void visit(const InternalSchemaTypeExpression* expr) override {
        visitPathExpression(expr);
    }

    void visit(const InternalSchemaUniqueItemsMatchExpression* expr) override {
        visitPathExpression(expr);
    }

    void visit(const InternalSchemaXorMatchExpression* expr) override {}

    void visit(const LTEMatchExpression* expr) override {
        visitPathExpression(expr);
    }

    void visit(const LTMatchExpression* expr) override {
        visitPathExpression(expr);
    }

    void visit(const ModMatchExpression* expr) override {
        visitPathExpression(expr);
    }

    void visit(const NorMatchExpression* expr) override {}

    void visit(const NotMatchExpression* expr) override {}

    void visit(const OrMatchExpression* expr) override {}

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

    void visit(const WhereMatchExpression* expr) override {
        _deps->needWholeDocument = true;
    }

    void visit(const WhereNoOpMatchExpression* expr) override {}

private:
    void visitPathExpression(const PathMatchExpression* expr) {
        if (_ignoreDependencies) {
            return;
        }

        if (auto path = expr->optPath()) {
            // If a path contains a numeric component then it should not be naively added to the
            // projection, since we do not support projecting specific array indices. Instead we add
            // the prefix of the path up to the numeric path component. Note that we start at path
            // component 1 rather than 0, because a numeric path component at the root of the
            // document can only ever be a field name, never an array index.
            FieldRef fieldRef(*path);
            for (size_t i = 1; i < fieldRef.numParts(); ++i) {
                if (fieldRef.isNumericPathComponentStrict(i)) {
                    auto prefix = fieldRef.dottedSubstring(0, i);
                    _deps->fields.insert(std::string{prefix});
                    return;
                }
            }

            _deps->fields.insert(std::string{*path});
        }
    }

    DepsTracker* _deps;
    bool& _ignoreDependencies;
};

class VariableRefVisitor : public SelectiveMatchExpressionVisitorBase<true> {
public:
    using SelectiveMatchExpressionVisitorBase<true>::visit;

    VariableRefVisitor(std::set<Variables::Id>* refs) : _refs(refs) {}

    void visit(const ExprMatchExpression* expr) override {
        if (expr->getExpression()) {
            expression::addVariableRefs(expr->getExpression().get(), _refs);
        }
    }

private:
    std::set<Variables::Id>* _refs;
};

}  // namespace

void addDependencies(const MatchExpression* expr, DepsTracker* deps) {
    bool ignoreDependencies = false;
    PostVisitor postVisitor(ignoreDependencies);
    DependencyVisitor visitor(deps, ignoreDependencies);
    MatchExpressionWalker walker(
        &visitor /*preVisitor*/, nullptr /*inVisitor*/, &postVisitor /*postVisitor*/);
    tree_walker::walk<true, MatchExpression>(expr, &walker);
}

void addVariableRefs(const MatchExpression* expr, std::set<Variables::Id>* refs) {
    VariableRefVisitor visitor(refs);
    MatchExpressionWalker walker(
        &visitor /*preVisitor*/, nullptr /*inVisitor*/, nullptr /*postVisitor*/);
    tree_walker::walk<true, MatchExpression>(expr, &walker);
}

}  // namespace mongo::dependency_analysis
