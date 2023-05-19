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

#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/query/projection_ast_util.h"
#include "mongo/db/query/projection_parser.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/query/query_shape.h"

namespace mongo::query_stats {

void addNonShapeObjCmdLiterals(BSONObjBuilder* bob,
                               const FindCommandRequest& findCommand,
                               const SerializationOptions& opts,
                               const boost::intrusive_ptr<ExpressionContext>& expCtx) {
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


BSONObj FindRequestShapifier::makeQueryStatsKey(const SerializationOptions& opts,
                                                OperationContext* opCtx) const {
    return makeQueryStatsKey(opts, makeDummyExpCtx(opCtx, _request));
}

BSONObj FindRequestShapifier::makeQueryStatsKey(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const ParsedFindCommand& parsedRequest,
    const SerializationOptions& opts) const {
    BSONObjBuilder bob;

    bob.append("queryShape", query_shape::extractQueryShape(parsedRequest, opts, expCtx));

    if (auto optObj = parsedRequest.findCommandRequest->getReadConcern()) {
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

    // Fields for literal redaction. Adds batchSize, maxTimeMS, and noCursorTimeOut.
    addNonShapeObjCmdLiterals(&bob, _request, opts, expCtx);
    if (_comment) {
        opts.appendLiteral(&bob, "comment", *_comment);
    }
    if (_applicationName.has_value()) {
        bob.append("applicationName", _applicationName.value());
    }

    return bob.obj();
}

BSONObj FindRequestShapifier::makeQueryStatsKey(
    const SerializationOptions& opts, const boost::intrusive_ptr<ExpressionContext>& expCtx) const {
    if (_initialQueryStatsKey && opts == SerializationOptions::kDefaultQueryShapeSerializeOptions) {
        auto tmp = std::move(*_initialQueryStatsKey);
        _initialQueryStatsKey = boost::none;
        return tmp;
    }
    // Note this makes a copy of the find command request since the shapifier outlives the request
    // in the query stats store.
    auto parsedRequest = uassertStatusOK(
        parsed_find_command::parse(expCtx,
                                   std::make_unique<FindCommandRequest>(_request),
                                   ExtensionsCallbackNoop(),
                                   MatchExpressionParser::kAllowAllSpecialFeatures));
    return makeQueryStatsKey(expCtx, *parsedRequest, opts);
}
}  // namespace mongo::query_stats
