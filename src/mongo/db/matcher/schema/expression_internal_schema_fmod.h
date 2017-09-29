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

#include "mongo/db/matcher/expression_leaf.h"

namespace mongo {

/**
 * MatchExpression for $_internalSchemaFmod keyword. Same as ModMatchExpression but works on
 * decimals.
 */
class InternalSchemaFmodMatchExpression final : public LeafMatchExpression {
public:
    InternalSchemaFmodMatchExpression() : LeafMatchExpression(MatchType::INTERNAL_SCHEMA_FMOD) {}

    Status init(StringData path, Decimal128 divisor, Decimal128 remainder);

    std::unique_ptr<MatchExpression> shallowClone() const final {
        std::unique_ptr<InternalSchemaFmodMatchExpression> m =
            stdx::make_unique<InternalSchemaFmodMatchExpression>();
        invariantOK(m->init(path(), _divisor, _remainder));
        if (getTag()) {
            m->setTag(getTag()->clone());
        }
        return std::move(m);
    }

    bool matchesSingleElement(const BSONElement& e, MatchDetails* details = nullptr) const final;

    void debugString(StringBuilder& debug, int level) const final;

    void serialize(BSONObjBuilder* out) const final;

    bool equivalent(const MatchExpression* other) const final;

    Decimal128 getDivisor() const {
        return _divisor;
    }
    Decimal128 getRemainder() const {
        return _remainder;
    }

private:
    ExpressionOptimizerFunc getOptimizer() const final {
        return [](std::unique_ptr<MatchExpression> expression) { return expression; };
    }

    Decimal128 _divisor;
    Decimal128 _remainder;
};
}  // namespace mongo
