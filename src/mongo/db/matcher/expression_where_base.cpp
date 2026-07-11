// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/expression_where_base.h"

#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/util/builder.h"

#include <utility>

namespace mongo {

WhereMatchExpressionBase::WhereMatchExpressionBase(WhereParams params)
    : MatchExpression(WHERE), _code(std::move(params.code)) {}

void WhereMatchExpressionBase::debugString(StringBuilder& debug, int indentationLevel) const {
    _debugAddSpace(debug, indentationLevel);
    debug << "$where";
    _debugStringAttachTagInfo(&debug);

    _debugAddSpace(debug, indentationLevel + 1);
    debug << "code: " << getCode() << "\n";
}

void WhereMatchExpressionBase::serialize(BSONObjBuilder* out,
                                         const query_shape::SerializationOptions& opts,
                                         bool includePath) const {
    opts.appendLiteral(out, "$where", BSONCode(getCode()));
}

bool WhereMatchExpressionBase::equivalent(const MatchExpression* other) const {
    if (matchType() != other->matchType()) {
        return false;
    }
    const WhereMatchExpressionBase* realOther = static_cast<const WhereMatchExpressionBase*>(other);
    return getCode() == realOther->getCode();
}

bool WhereMatchExpressionBase::evaluateWherePredicate(const WhereMatchExpressionBase* expr,
                                                      const BSONObj& doc) {
    return expr->runPredicate(doc);
}
}  // namespace mongo
