/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/db/matcher/expression_path.h"

namespace mongo {

class InternalSchemaObjectMatchExpression final : public PathMatchExpression {
public:
    static constexpr StringData kName = "$_internalSchemaObjectMatch"_sd;

    InternalSchemaObjectMatchExpression()
        : PathMatchExpression(INTERNAL_SCHEMA_OBJECT_MATCH,
                              ElementPath::LeafArrayBehavior::kNoTraversal,
                              ElementPath::NonLeafArrayBehavior::kTraverse) {}

    Status init(std::unique_ptr<MatchExpression> expr, StringData path) {
        _sub = std::move(expr);
        return setPath(path);
    }

    bool matchesSingleElement(const BSONElement& elem, MatchDetails* details = nullptr) const final;

    std::unique_ptr<MatchExpression> shallowClone() const final;

    void debugString(StringBuilder& debug, int level = 0) const final;

    void serialize(BSONObjBuilder* out) const final;

    bool equivalent(const MatchExpression* other) const final;

    std::vector<MatchExpression*>* getChildVector() final {
        return nullptr;
    }

    size_t numChildren() const final {
        invariant(_sub);
        return 1;
    }

    MatchExpression* getChild(size_t i) const final {
        // 'i' must be 0 since there's always exactly one child.
        invariant(i == 0);
        return _sub.get();
    }

    MatchCategory getCategory() const final {
        return MatchCategory::kOther;
    }

private:
    ExpressionOptimizerFunc getOptimizer() const final;

    std::unique_ptr<MatchExpression> _sub;
};
}  // namespace mongo
