// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/schema/expression_internal_schema_root_doc_eq.h"

#include "mongo/bson/util/builder.h"

#include <string_view>

namespace mongo {

constexpr std::string_view InternalSchemaRootDocEqMatchExpression::kName;

void InternalSchemaRootDocEqMatchExpression::debugString(StringBuilder& debug,
                                                         int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);
    debug << kName << " " << _rhsObj.toString();
    _debugStringAttachTagInfo(&debug);
}

void InternalSchemaRootDocEqMatchExpression::serialize(
    BSONObjBuilder* out, const query_shape::SerializationOptions& opts, bool includePath) const {
    BSONObjBuilder subObj(out->subobjStart(kName));
    query_shape::SerializationOptions options = opts;
    options.addHmacedObjToBuilder(&subObj, _rhsObj);
    subObj.doneFast();
}

bool InternalSchemaRootDocEqMatchExpression::equivalent(const MatchExpression* other) const {
    if (matchType() != other->matchType()) {
        return false;
    }

    auto realOther = static_cast<const InternalSchemaRootDocEqMatchExpression*>(other);
    return _objCmp.evaluate(_rhsObj == realOther->_rhsObj);
}

std::unique_ptr<MatchExpression> InternalSchemaRootDocEqMatchExpression::clone() const {
    auto clone =
        std::make_unique<InternalSchemaRootDocEqMatchExpression>(_rhsObj.copy(), _errorAnnotation);
    if (getTag()) {
        clone->setTag(getTag()->clone());
    }
    return std::move(clone);
}

}  // namespace mongo
