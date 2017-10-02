/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include "mongo/bson/unordered_fields_bsonobj_comparator.h"
#include "mongo/db/matcher/expression.h"

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

    InternalSchemaRootDocEqMatchExpression()
        : MatchExpression(MatchExpression::INTERNAL_SCHEMA_ROOT_DOC_EQ) {}

    void init(BSONObj obj) {
        _rhsObj = std::move(obj);
    }

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

    void debugString(StringBuilder& debug, int level = 0) const final;

    void serialize(BSONObjBuilder* out) const final;

    bool equivalent(const MatchExpression* other) const final;

    size_t numChildren() const final {
        return 0;
    }

    MatchExpression* getChild(size_t i) const final {
        MONGO_UNREACHABLE;
    }

    std::vector<MatchExpression*>* getChildVector() final {
        return nullptr;
    }

    MatchCategory getCategory() const final {
        return MatchCategory::kOther;
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
