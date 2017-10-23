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

#include "mongo/platform/basic.h"

#include "mongo/db/matcher/schema/expression_internal_schema_match_array_index.h"

namespace mongo {
constexpr StringData InternalSchemaMatchArrayIndexMatchExpression::kName;

void InternalSchemaMatchArrayIndexMatchExpression::debugString(StringBuilder& debug,
                                                               int level) const {
    _debugAddSpace(debug, level);

    BSONObjBuilder builder;
    serialize(&builder);
    debug << builder.obj().toString() << "\n";

    const auto* tag = getTag();
    if (tag) {
        debug << " ";
        tag->debugString(&debug);
    }
    debug << "\n";
}

bool InternalSchemaMatchArrayIndexMatchExpression::equivalent(const MatchExpression* expr) const {
    if (matchType() != expr->matchType()) {
        return false;
    }

    const auto* other = static_cast<const InternalSchemaMatchArrayIndexMatchExpression*>(expr);
    return path() == other->path() && _index == other->_index &&
        _expression->equivalent(other->_expression.get());
}

Status InternalSchemaMatchArrayIndexMatchExpression::init(
    StringData path, long long index, std::unique_ptr<ExpressionWithPlaceholder> expression) {
    invariant(static_cast<bool>(expression));
    _index = index;
    _expression = std::move(expression);
    return setPath(path);
}

void InternalSchemaMatchArrayIndexMatchExpression::serialize(BSONObjBuilder* builder) const {
    BSONObjBuilder pathSubobj(builder->subobjStart(path()));
    {
        BSONObjBuilder matchArrayElemSubobj(pathSubobj.subobjStart(kName));
        matchArrayElemSubobj.append("index", _index);
        matchArrayElemSubobj.append("namePlaceholder", _expression->getPlaceholder().value_or(""));
        {
            BSONObjBuilder subexprSubObj(matchArrayElemSubobj.subobjStart("expression"));
            _expression->getFilter()->serialize(&subexprSubObj);
            subexprSubObj.doneFast();
        }
        matchArrayElemSubobj.doneFast();
    }
    pathSubobj.doneFast();
}

std::unique_ptr<MatchExpression> InternalSchemaMatchArrayIndexMatchExpression::shallowClone()
    const {
    auto clone = stdx::make_unique<InternalSchemaMatchArrayIndexMatchExpression>();
    invariantOK(clone->init(path(), _index, _expression->shallowClone()));
    return std::move(clone);
}

MatchExpression::ExpressionOptimizerFunc
InternalSchemaMatchArrayIndexMatchExpression::getOptimizer() const {
    return [](std::unique_ptr<MatchExpression> expression) {
        static_cast<InternalSchemaMatchArrayIndexMatchExpression&>(*expression)
            ._expression->optimizeFilter();
        return expression;
    };
}
}  // namespace mongo
