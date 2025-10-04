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

#include "mongo/db/matcher/expression_where.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
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
