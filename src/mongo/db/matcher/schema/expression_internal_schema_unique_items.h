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
#include <utility>

#include "mongo/bson/unordered_fields_bsonelement_comparator.h"
#include "mongo/db/matcher/expression_array.h"

namespace mongo {

/**
 * Matches arrays whose elements are all unique. When comparing elements,
 *
 *  - strings are always compared using the "simple" string comparator; and
 *  - objects are compared in a field order-independent manner.
 */
class InternalSchemaUniqueItemsMatchExpression final : public ArrayMatchingMatchExpression {
public:
    static constexpr StringData kName = "$_internalSchemaUniqueItems"_sd;

    explicit InternalSchemaUniqueItemsMatchExpression(
        StringData path, clonable_ptr<ErrorAnnotation> annotation = nullptr)
        : ArrayMatchingMatchExpression(
              MatchExpression::INTERNAL_SCHEMA_UNIQUE_ITEMS, path, std::move(annotation)) {}

    size_t numChildren() const final {
        return 0;
    }

    MatchExpression* getChild(size_t i) const final {
        MONGO_UNREACHABLE_TASSERT(6400219);
    }

    void resetChild(size_t i, MatchExpression*) final override {
        MONGO_UNREACHABLE;
    }

    std::vector<std::unique_ptr<MatchExpression>>* getChildVector() final {
        return nullptr;
    }

    bool matchesArray(const BSONObj& array, MatchDetails*) const final {
        return !findFirstDuplicateValue(array);
    }

    BSONElement findFirstDuplicateValue(const BSONObj& array) const {
        auto set = _comparator.makeBSONEltSet();
        for (auto&& elem : array) {
            if (!std::get<bool>(set.insert(elem))) {
                return elem;
            }
        }
        return {};
    }

    void debugString(StringBuilder& builder, int indentationLevel) const final;

    bool equivalent(const MatchExpression* other) const final;

    BSONObj getSerializedRightHandSide() const final;

    std::unique_ptr<MatchExpression> shallowClone() const final;

    void acceptVisitor(MatchExpressionMutableVisitor* visitor) final {
        visitor->visit(this);
    }

    void acceptVisitor(MatchExpressionConstVisitor* visitor) const final {
        visitor->visit(this);
    }

private:
    ExpressionOptimizerFunc getOptimizer() const final {
        return [](std::unique_ptr<MatchExpression> expression) { return expression; };
    }

    // The comparator to use when comparing BSONElements, which will never use a collation.
    UnorderedFieldsBSONElementComparator _comparator;
};
}  // namespace mongo
