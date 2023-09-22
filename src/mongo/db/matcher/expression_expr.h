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

#pragma once

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "mongo/base/clonable_ptr.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/expression_visitor.h"
#include "mongo/db/matcher/match_details.h"
#include "mongo/db/matcher/matchable.h"
#include "mongo/db/matcher/rewrite_expr.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_walker.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/expression_walker.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/string_map.h"

namespace mongo {

/**
 * MatchExpression for the top-level $expr keyword. Take an expression as an argument, evaluates and
 * coerces to boolean form which determines whether a document is a match.
 */
class ExprMatchExpression final : public MatchExpression {
public:
    ExprMatchExpression(BSONElement elem,
                        const boost::intrusive_ptr<ExpressionContext>& expCtx,
                        clonable_ptr<ErrorAnnotation> annotation = nullptr);

    ExprMatchExpression(boost::intrusive_ptr<Expression> expr,
                        const boost::intrusive_ptr<ExpressionContext>& expCtx,
                        clonable_ptr<ErrorAnnotation> annotation = nullptr);

    bool matchesSingleElement(const BSONElement& e, MatchDetails* details = nullptr) const final {
        MONGO_UNREACHABLE;
    }

    bool matches(const MatchableDocument* doc, MatchDetails* details = nullptr) const final;

    /**
     * Evaluates the aggregation expression of this match expression on document 'doc' and returns
     * the result.
     */
    Value evaluateExpression(const MatchableDocument* doc) const;

    std::unique_ptr<MatchExpression> clone() const final;

    void debugString(StringBuilder& debug, int indentationLevel = 0) const final {
        _debugAddSpace(debug, indentationLevel);
        debug << "$expr " << _expression->serialize(SerializationOptions{}).toString();
        _debugStringAttachTagInfo(&debug);
    }

    void serialize(BSONObjBuilder* out, const SerializationOptions& opts) const final;

    bool isTriviallyTrue() const final;

    bool equivalent(const MatchExpression* other) const final;

    MatchCategory getCategory() const final {
        return MatchCategory::kOther;
    }

    size_t numChildren() const final {
        return 0;
    }

    MatchExpression* getChild(size_t i) const final {
        MONGO_UNREACHABLE_TASSERT(6400207);
    }

    void resetChild(size_t, MatchExpression*) {
        MONGO_UNREACHABLE;
    };

    std::vector<std::unique_ptr<MatchExpression>>* getChildVector() final {
        return nullptr;
    }

    boost::intrusive_ptr<ExpressionContext> getExpressionContext() const {
        return _expCtx;
    }

    boost::intrusive_ptr<Expression> getExpression() const {
        return _expression;
    }

    /**
     * Use if the caller needs to modify the expression held by this $expr.
     */
    boost::intrusive_ptr<Expression>& getExpressionRef() {
        return _expression;
    }

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }

    /**
     * Finds an applicable rename from 'renameList' (if one exists) and applies it to the
     * expression. Each pair in 'renameList' specifies a path prefix that should be renamed (as the
     * first element) and the path components that should replace the renamed prefix (as the second
     * element).
     */
    void applyRename(const StringMap<std::string>& renameList) {
        SubstituteFieldPathWalker substituteWalker(renameList);
        expression_walker::walk<Expression>(_expression.get(), &substituteWalker);
    }

    bool hasRenameablePath(const StringMap<std::string>& renameList) const {
        bool hasRenameablePath = false;
        FieldPathVisitor visitor([&](const ExpressionFieldPath* expr) {
            hasRenameablePath =
                hasRenameablePath || expr->isRenameableByAnyPrefixNameIn(renameList);
        });
        stage_builder::ExpressionWalker walker(
            &visitor, nullptr /*inVisitor*/, nullptr /*postVisitor*/);
        expression_walker::walk(_expression.get(), &walker);
        return hasRenameablePath;
    }

private:
    ExpressionOptimizerFunc getOptimizer() const final;

    void _doSetCollator(const CollatorInterface* collator) final;

    boost::intrusive_ptr<ExpressionContext> _expCtx;

    boost::intrusive_ptr<Expression> _expression;

    boost::optional<RewriteExpr::RewriteResult> _rewriteResult;
};

}  // namespace mongo
