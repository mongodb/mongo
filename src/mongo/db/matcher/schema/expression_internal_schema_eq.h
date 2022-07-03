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

#include "mongo/bson/unordered_fields_bsonelement_comparator.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/util/assert_util.h"

namespace mongo {

/**
 * MatchExpression for $_internalSchemaEq, which behaves similar to $eq except:
 *
 * - leaf arrays are not traversed.
 * - comparisons between objects do not consider field order.
 * - null element values only match the literal null, and not missing or undefined values.
 * - always uses simple string comparison semantics, even if the query has a non-simple collation.
 */
class InternalSchemaEqMatchExpression final : public LeafMatchExpression {
public:
    static constexpr StringData kName = "$_internalSchemaEq"_sd;

    InternalSchemaEqMatchExpression(StringData path,
                                    BSONElement rhs,
                                    clonable_ptr<ErrorAnnotation> annotation = nullptr);

    std::unique_ptr<MatchExpression> shallowClone() const final;

    bool matchesSingleElement(const BSONElement&, MatchDetails*) const final;

    void debugString(StringBuilder& debug, int indentationLevel) const final;

    BSONObj getSerializedRightHandSide() const final;

    bool equivalent(const MatchExpression* other) const final;

    size_t numChildren() const final {
        return 0;
    }

    MatchExpression* getChild(size_t i) const final {
        MONGO_UNREACHABLE_TASSERT(6400213);
    }

    void resetChild(size_t, MatchExpression*) override {
        MONGO_UNREACHABLE;
    }

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

    UnorderedFieldsBSONElementComparator _eltCmp;
    BSONElement _rhsElem;
};
}  // namespace mongo
