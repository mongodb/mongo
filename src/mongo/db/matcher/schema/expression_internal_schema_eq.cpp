// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/schema/expression_internal_schema_eq.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/matcher/path.h"

#include <string_view>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

constexpr std::string_view InternalSchemaEqMatchExpression::kName;

InternalSchemaEqMatchExpression::InternalSchemaEqMatchExpression(
    boost::optional<std::string_view> path,
    BSONElement rhs,
    clonable_ptr<ErrorAnnotation> annotation)
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
    BSONObjBuilder* bob, const query_shape::SerializationOptions& opts, bool includePath) const {
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
