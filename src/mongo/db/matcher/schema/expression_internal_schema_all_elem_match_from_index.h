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

#include "mongo/base/string_data.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_with_placeholder.h"
#include "mongo/db/matcher/match_expression_util.h"

namespace mongo {
/**
 * A match expression similar to $elemMatch, but only matches arrays for which every element
 * matches the sub-expression.
 */
class InternalSchemaAllElemMatchFromIndexMatchExpression final
    : public ArrayMatchingMatchExpression {
public:
    static constexpr StringData kName = "$_internalSchemaAllElemMatchFromIndex"_sd;
    static constexpr int kNumChildren = 1;

    InternalSchemaAllElemMatchFromIndexMatchExpression(
        StringData path,
        long long index,
        std::unique_ptr<ExpressionWithPlaceholder> expression,
        clonable_ptr<ErrorAnnotation> annotation = nullptr);

    std::unique_ptr<MatchExpression> shallowClone() const final;

    bool matchesArray(const BSONObj& array, MatchDetails* details) const final {
        return !findFirstMismatchInArray(array, details);
    }

    /**
     * Finds the first element in the sub-array of array 'array' that the expression applies to that
     * does not match the sub-expression. If such element does not exist, then returns empty (i.e.
     * EOO) value.
     */
    BSONElement findFirstMismatchInArray(const BSONObj& array, MatchDetails* details) const {
        auto iter = BSONObjIterator(array);
        match_expression_util::advanceBy(_index, iter);
        while (iter.more()) {
            auto element = iter.next();
            if (!_expression->matchesBSONElement(element, details)) {
                return element;
            }
        }
        return {};
    }

    void debugString(StringBuilder& debug, int indentationLevel) const final;

    BSONObj getSerializedRightHandSide() const final;

    bool equivalent(const MatchExpression* other) const final;

    /**
     * Returns an index of the first element of the array this match expression applies to.
     */
    long long startIndex() const {
        return _index;
    }

    std::vector<std::unique_ptr<MatchExpression>>* getChildVector() final {
        return nullptr;
    }

    size_t numChildren() const final {
        return kNumChildren;
    }

    MatchExpression* getChild(size_t i) const final {
        tassert(6400200, "Out-of-bounds access to child of MatchExpression.", i < kNumChildren);
        return _expression->getFilter();
    }

    void resetChild(size_t i, MatchExpression* other) {
        tassert(6329407, "Out-of-bounds access to child of MatchExpression.", i < kNumChildren);
        _expression->resetFilter(other);
    };


    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }

private:
    ExpressionOptimizerFunc getOptimizer() const final;

    long long _index;
    std::unique_ptr<ExpressionWithPlaceholder> _expression;
};
}  // namespace mongo
