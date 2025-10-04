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

#include "mongo/db/matcher/schema/expression_internal_schema_eq.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/matcher/path.h"

#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

constexpr StringData InternalSchemaEqMatchExpression::kName;

InternalSchemaEqMatchExpression::InternalSchemaEqMatchExpression(
    boost::optional<StringData> path, BSONElement rhs, clonable_ptr<ErrorAnnotation> annotation)
    : LeafMatchExpression(MatchType::INTERNAL_SCHEMA_EQ,
                          path,
                          ElementPath::LeafArrayBehavior::kNoTraversal,
                          ElementPath::NonLeafArrayBehavior::kTraverse,
                          std::move(annotation)),
      _rhsElem(rhs) {
    invariant(_rhsElem);
}

void InternalSchemaEqMatchExpression::debugString(StringBuilder& debug,
                                                  int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);
    debug << path() << " " << kName << " " << _rhsElem.toString(false);
    _debugStringAttachTagInfo(&debug);
}

void InternalSchemaEqMatchExpression::appendSerializedRightHandSide(
    BSONObjBuilder* bob, const SerializationOptions& opts, bool includePath) const {
    if (!opts.isKeepingLiteralsUnchanged() && _rhsElem.isABSONObj()) {
        BSONObjBuilder exprSpec(bob->subobjStart(kName));
        opts.addHmacedObjToBuilder(&exprSpec, _rhsElem.Obj());
        exprSpec.doneFast();
        return;
    }
    // If the element is not an object it must be a literal.
    opts.appendLiteral(bob, kName, _rhsElem);
}

bool InternalSchemaEqMatchExpression::equivalent(const MatchExpression* other) const {
    if (other->matchType() != matchType()) {
        return false;
    }

    auto realOther = static_cast<const InternalSchemaEqMatchExpression*>(other);
    return path() == realOther->path() && _eltCmp.evaluate(_rhsElem == realOther->_rhsElem);
}

std::unique_ptr<MatchExpression> InternalSchemaEqMatchExpression::clone() const {
    auto clone =
        std::make_unique<InternalSchemaEqMatchExpression>(path(), _rhsElem, _errorAnnotation);
    if (getTag()) {
        clone->setTag(getTag()->clone());
    }
    return clone;
}

}  //  namespace mongo
