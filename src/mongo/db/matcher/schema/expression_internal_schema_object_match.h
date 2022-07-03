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

#include "mongo/db/matcher/expression_path.h"

namespace mongo {

class InternalSchemaObjectMatchExpression final : public PathMatchExpression {
public:
    static constexpr StringData kName = "$_internalSchemaObjectMatch"_sd;
    static constexpr int kNumChildren = 1;

    InternalSchemaObjectMatchExpression(StringData path,
                                        std::unique_ptr<MatchExpression> expr,
                                        clonable_ptr<ErrorAnnotation> annotation = nullptr);

    bool matchesSingleElement(const BSONElement& elem, MatchDetails* details = nullptr) const final;

    std::unique_ptr<MatchExpression> shallowClone() const final;

    void debugString(StringBuilder& debug, int indentationLevel = 0) const final;

    BSONObj getSerializedRightHandSide() const final;

    bool equivalent(const MatchExpression* other) const final;

    std::vector<std::unique_ptr<MatchExpression>>* getChildVector() final {
        return nullptr;
    }

    size_t numChildren() const final {
        invariant(_sub);
        return kNumChildren;
    }

    MatchExpression* getChild(size_t i) const final {
        // 'i' must be 0 since there's always exactly one child.
        tassert(6400217, "Out-of-bounds access to child of MatchExpression.", i < kNumChildren);
        return _sub.get();
    }

    void resetChild(size_t i, MatchExpression* other) final override {
        tassert(6329410, "Out-of-bounds access to child of MatchExpression.", i < kNumChildren);
        _sub.reset(other);
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

private:
    ExpressionOptimizerFunc getOptimizer() const final;

    std::unique_ptr<MatchExpression> _sub;
};
}  // namespace mongo
