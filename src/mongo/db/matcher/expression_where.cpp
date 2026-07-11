// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/matcher/expression_where.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/db/database_name.h"
#include "mongo/db/exec/js_function.h"
#include "mongo/db/matcher/expression.h"

#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>


namespace mongo {

using std::unique_ptr;

WhereMatchExpression::WhereMatchExpression(OperationContext* opCtx,
                                           WhereParams params,
                                           const DatabaseName& dbName)
    : WhereMatchExpressionBase(std::move(params)),
      _opCtx(opCtx),
      _jsFunction(std::make_unique<JsFunction>(_opCtx, getCode(), dbName)) {}

WhereMatchExpression::~WhereMatchExpression() = default;

const JsFunction& WhereMatchExpression::getPredicate() const {
    validateState();
    return *_jsFunction;
}

std::unique_ptr<JsFunction> WhereMatchExpression::extractPredicate() {
    return std::move(_jsFunction);
}

void WhereMatchExpression::setPredicate(std::unique_ptr<JsFunction> jsFunction) {
    tassert(8415200, "JsFunction must not be set", !_jsFunction);
    _jsFunction = std::move(jsFunction);
}

void WhereMatchExpression::validateState() const {
    tassert(6403600, "JsFunction is unavailable", _jsFunction);
}

bool WhereMatchExpression::runPredicate(const BSONObj& doc) const {
    return getPredicate().runAsPredicate(doc);
}

unique_ptr<MatchExpression> WhereMatchExpression::clone() const {
    validateState();

    WhereParams params;
    params.code = getCode();
    unique_ptr<WhereMatchExpression> e =
        std::make_unique<WhereMatchExpression>(_opCtx, std::move(params), _jsFunction->getDbName());
    if (getTag()) {
        e->setTag(getTag()->clone());
    }
    if (getInputParamId()) {
        e->setInputParamId(*getInputParamId());
    }
    return e;
}
}  // namespace mongo
