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

    InternalSchemaUniqueItemsMatchExpression()
        : ArrayMatchingMatchExpression(MatchExpression::INTERNAL_SCHEMA_UNIQUE_ITEMS) {}

    Status init(StringData path) {
        return setPath(path);
    }

    size_t numChildren() const final {
        return 0;
    }

    MatchExpression* getChild(size_t i) const final {
        MONGO_UNREACHABLE;
    }

    std::vector<MatchExpression*>* getChildVector() final {
        return nullptr;
    }

    bool matchesArray(const BSONObj& array, MatchDetails*) const final {
        auto set = _comparator.makeBSONEltSet();
        for (auto&& elem : array) {
            if (!std::get<bool>(set.insert(elem))) {
                return false;
            }
        }
        return true;
    }

    void debugString(StringBuilder& builder, int level) const final;

    bool equivalent(const MatchExpression* other) const final;

    void serialize(BSONObjBuilder* builder) const final;

    std::unique_ptr<MatchExpression> shallowClone() const final;

private:
    ExpressionOptimizerFunc getOptimizer() const final {
        return [](std::unique_ptr<MatchExpression> expression) { return expression; };
    }

    // The comparator to use when comparing BSONElements, which will never use a collation.
    UnorderedFieldsBSONElementComparator _comparator;
};
}  // namespace mongo
