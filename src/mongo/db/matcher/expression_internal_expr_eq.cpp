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

#include "mongo/db/matcher/expression_internal_expr_eq.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"

namespace mongo {

constexpr StringData InternalExprEqMatchExpression::kName;

bool InternalExprEqMatchExpression::matchesSingleElement(const BSONElement& elem,
                                                         MatchDetails* details) const {
    // We use NonLeafArrayBehavior::kMatchSubpath traversal in InternalExprEqMatchExpression. This
    // means matchesSinglElement() will be called when an array is found anywhere along the patch we
    // are matching against. When this occurs, we return 'true' and depend on the corresponding
    // ExprMatchExpression node to filter properly.
    if (elem.type() == BSONType::Array) {
        return true;
    }

    if (elem.canonicalType() != _rhs.canonicalType()) {
        return false;
    }

    auto comp = BSONElement::compareElements(
        elem, _rhs, BSONElement::ComparisonRules::kConsiderFieldName, _collator);
    return comp == 0;
}

std::unique_ptr<MatchExpression> InternalExprEqMatchExpression::shallowClone() const {
    auto clone = stdx::make_unique<InternalExprEqMatchExpression>();
    invariantOK(clone->init(path(), _rhs));
    clone->setCollator(_collator);
    if (getTag()) {
        clone->setTag(getTag()->clone());
    }
    return std::move(clone);
}

}  //  namespace mongo
