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

#include <boost/optional.hpp>

#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/util/assert_util.h"

namespace mongo {

/**
 * MatchExpression for $_internalSchemaRootDocEq, which matches the root document with the
 * following equality semantics:
 *
 * - comparisons between objects do not consider field order.
 * - null element values only match the literal null, and not missing or undefined values.
 * - always uses simple string comparison semantics, even if the query has a non-simple collation.
 */
class InternalSchemaRootDocEqMatchExpression final : public MatchExpression {
public:
    static constexpr StringData kName = "$_internalSchemaRootDocEq"_sd;

    /**
     * Constructs a new match expression, taking ownership of 'rhs'.
     */
    explicit InternalSchemaRootDocEqMatchExpression(
        BSONObj rhs, clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : MatchExpression(MatchExpression::INTERNAL_SCHEMA_ROOT_DOC_EQ, std::move(annotation)),
          _rhsObj(std::move(rhs)) {}

    bool matches(const MatchableDocument* doc, MatchDetails* details = nullptr) const final;

    /**
     * This expression should only be used to match full documents, not objects within an array
     * in the case of $elemMatch.
     */
    bool matchesSingleElement(const BSONElement& elem,
                              MatchDetails* details = nullptr) const final {
        MONGO_UNREACHABLE;
    }

    std::unique_ptr<MatchExpression> shallowClone() const final;

    void debugString(StringBuilder& debug, int indentationLevel = 0) const final;

    void serialize(BSONObjBuilder* out, bool includePath) const final;

    bool equivalent(const MatchExpression* other) const final;

    size_t numChildren() const final {
        return 0;
    }

    MatchExpression* getChild(size_t i) const final {
        MONGO_UNREACHABLE_TASSERT(6400218);
    }

    void resetChild(size_t i, MatchExpression*) final override {
        MONGO_UNREACHABLE;
    }

    std::vector<std::unique_ptr<MatchExpression>>* getChildVector() final {
        return nullptr;
    }

    MatchCategory getCategory() const final {
        return MatchCategory::kOther;
    }

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }

protected:
    void _doAddDependencies(DepsTracker* deps) const final {
        deps->needWholeDocument = true;
    }

private:
    ExpressionOptimizerFunc getOptimizer() const final {
        return [](std::unique_ptr<MatchExpression> expression) { return expression; };
    }

    UnorderedFieldsBSONObjComparator _objCmp;
    BSONObj _rhsObj;
};
}  // namespace mongo
