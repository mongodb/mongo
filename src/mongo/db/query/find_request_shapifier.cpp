/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/query/find_request_shapifier.h"

#include "mongo/db/query/projection_ast_util.h"
#include "mongo/db/query/projection_parser.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/query/query_shape.h"

namespace mongo::telemetry {

void addNonShapeObjCmdLiterals(BSONObjBuilder* bob,
                               const FindCommandRequest& findCommand,
                               const SerializationOptions& opts,
                               const boost::intrusive_ptr<ExpressionContext>& expCtx) {

    if (const auto& comment = expCtx->opCtx->getComment()) {
        opts.appendLiteral(bob, "comment", *comment);
    }

    if (auto noCursorTimeout = findCommand.getNoCursorTimeout()) {
        // Capture whether noCursorTimeout was specified in the query, do not distinguish between
        // true or false.
        opts.appendLiteral(
            bob, FindCommandRequest::kNoCursorTimeoutFieldName, noCursorTimeout.has_value());
    }

    if (auto maxTimeMs = findCommand.getMaxTimeMS()) {
        opts.appendLiteral(bob, FindCommandRequest::kMaxTimeMSFieldName, *maxTimeMs);
    }

    if (auto batchSize = findCommand.getBatchSize()) {
        opts.appendLiteral(
            bob, FindCommandRequest::kBatchSizeFieldName, static_cast<long long>(*batchSize));
    }
}


BSONObj FindRequestShapifier::makeTelemetryKey(const SerializationOptions& opts,
                                               OperationContext* opCtx) const {
    auto expCtx = make_intrusive<ExpressionContext>(
        opCtx, _request, nullptr /* collator doesn't matter here.*/, false /* mayDbProfile */);
    expCtx->maxFeatureCompatibilityVersion = boost::none;  // Ensure all features are allowed.
    // Expression counters are reported in serverStatus to indicate how often clients use certain
    // expressions/stages, so it's a side effect tied to parsing. We must stop expression counters
    // before re-parsing to avoid adding to the counters more than once per a given query.
    expCtx->stopExpressionCounters();
    return makeTelemetryKey(opts, expCtx);
}

BSONObj FindRequestShapifier::makeTelemetryKey(
    const SerializationOptions& opts, const boost::intrusive_ptr<ExpressionContext>& expCtx) const {
    BSONObjBuilder bob;

    bob.append("queryShape", query_shape::extractQueryShape(_request, opts, expCtx));

    if (auto optObj = _request.getReadConcern()) {
        // Read concern should not be considered a literal.
        bob.append(FindCommandRequest::kReadConcernFieldName, optObj.get());
    }
    // has_value() returns true if allowParitalResults was populated by the original query.
    if (_request.getAllowPartialResults().has_value()) {
        // Note we are intentionally avoiding opts.appendLiteral() here and want to keep the exact
        // value. value_or() will return the stored value, or the default that is passed in. Since
        // we've already checked that allowPartialResults has a stored value, the default will never
        // be used.
        bob.append(FindCommandRequest::kAllowPartialResultsFieldName,
                   _request.getAllowPartialResults().value_or(false));
    }

    // Fields for literal redaction. Adds comment, batchSize, maxTimeMS, and noCursorTimeOut.
    addNonShapeObjCmdLiterals(&bob, _request, opts, expCtx);

    if (_applicationName.has_value()) {
        bob.append("applicationName", _applicationName.value());
    }


    return bob.obj();
}
}  // namespace mongo::telemetry
