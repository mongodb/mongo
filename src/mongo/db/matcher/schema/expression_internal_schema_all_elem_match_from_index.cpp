/**
 *    Copyright (C) 2017 10gen Inc.
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
#include "mongo/platform/basic.h"

#include "mongo/db/matcher/schema/expression_internal_schema_all_elem_match_from_index.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"

namespace mongo {

constexpr StringData InternalSchemaAllElemMatchFromIndexMatchExpression::kName;

std::unique_ptr<MatchExpression> InternalSchemaAllElemMatchFromIndexMatchExpression::shallowClone()
    const {
    auto clone = stdx::make_unique<InternalSchemaAllElemMatchFromIndexMatchExpression>();
    invariantOK(clone->init(path(), _index, _expression->shallowClone()));
    if (getTag()) {
        clone->setTag(getTag()->clone());
    }
    return std::move(clone);
}

bool InternalSchemaAllElemMatchFromIndexMatchExpression::equivalent(
    const MatchExpression* other) const {
    if (matchType() != other->matchType()) {
        return false;
    }
    const InternalSchemaAllElemMatchFromIndexMatchExpression* realOther =
        static_cast<const InternalSchemaAllElemMatchFromIndexMatchExpression*>(other);
    return (_index == realOther->_index && _expression->equivalent(realOther->_expression.get()));
}

void InternalSchemaAllElemMatchFromIndexMatchExpression::debugString(StringBuilder& debug,
                                                                     int level) const {
    _debugAddSpace(debug, level);
    debug << kName << "\n";
    debug << " index: " << _index << ", query:\n";
    _expression->getFilter()->debugString(debug, level + 1);
}

void InternalSchemaAllElemMatchFromIndexMatchExpression::serialize(BSONObjBuilder* out) const {
    BSONObjBuilder allElemMatchBob(out->subobjStart(path()));
    BSONArrayBuilder subArray(allElemMatchBob.subarrayStart(kName));
    subArray.append(_index);
    {
        BSONObjBuilder eBuilder(subArray.subobjStart());
        _expression->getFilter()->serialize(&eBuilder);
        eBuilder.doneFast();
    }
    subArray.doneFast();
    allElemMatchBob.doneFast();
}

MatchExpression::ExpressionOptimizerFunc
InternalSchemaAllElemMatchFromIndexMatchExpression::getOptimizer() const {
    return [](std::unique_ptr<MatchExpression> expression) {
        static_cast<InternalSchemaAllElemMatchFromIndexMatchExpression&>(*expression)
            ._expression->optimizeFilter();
        return expression;
    };
}
}  //  namespace mongo
