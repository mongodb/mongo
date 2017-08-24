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

#include "mongo/db/matcher/schema/expression_internal_schema_eq.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"

namespace mongo {

constexpr StringData InternalSchemaEqMatchExpression::kName;

Status InternalSchemaEqMatchExpression::init(StringData path, BSONElement rhs) {
    invariant(rhs);
    _rhsElem = rhs;
    return setPath(path);
}

bool InternalSchemaEqMatchExpression::matchesSingleElement(const BSONElement& elem,
                                                           MatchDetails* details) const {
    return _eltCmp.evaluate(_rhsElem == elem);
}

void InternalSchemaEqMatchExpression::debugString(StringBuilder& debug, int level) const {
    _debugAddSpace(debug, level);
    debug << path() << " " << kName << " " << _rhsElem.toString(false);

    auto td = getTag();
    if (td) {
        debug << " ";
        td->debugString(&debug);
    }

    debug << "\n";
}

void InternalSchemaEqMatchExpression::serialize(BSONObjBuilder* out) const {
    BSONObjBuilder eqObj(out->subobjStart(path()));
    eqObj.appendAs(_rhsElem, kName);
    eqObj.doneFast();
}

bool InternalSchemaEqMatchExpression::equivalent(const MatchExpression* other) const {
    if (other->matchType() != matchType()) {
        return false;
    }

    auto realOther = static_cast<const InternalSchemaEqMatchExpression*>(other);
    return path() == realOther->path() && _eltCmp.evaluate(_rhsElem == realOther->_rhsElem);
}

std::unique_ptr<MatchExpression> InternalSchemaEqMatchExpression::shallowClone() const {
    auto clone = stdx::make_unique<InternalSchemaEqMatchExpression>();
    invariantOK(clone->init(path(), _rhsElem));
    if (getTag()) {
        clone->setTag(getTag()->clone());
    }
    return std::move(clone);
}

}  //  namespace mongo
