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

#include "mongo/db/query/aggregate_request_shapifier.h"

#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/query_shape.h"

namespace mongo::query_stats {

BSONObj AggregateRequestShapifier::makeQueryStatsKey(const SerializationOptions& opts,
                                                     OperationContext* opCtx) const {
    return makeQueryStatsKey(opts, makeDummyExpCtx(opCtx));
}

BSONObj AggregateRequestShapifier::makeQueryStatsKey(
    const SerializationOptions& opts, const boost::intrusive_ptr<ExpressionContext>& expCtx) const {
    if (_initialQueryStatsKey && !opts.applyHmacToIdentifiers &&
        opts.literalPolicy == LiteralSerializationPolicy::kToDebugTypeString) {
        auto tmp = std::move(*_initialQueryStatsKey);
        _initialQueryStatsKey = boost::none;
        return tmp;
    }

    auto pipeline = Pipeline::parse(_request.getPipeline(), expCtx);
    return _makeQueryStatsKeyHelper(opts, expCtx, *pipeline);
}

BSONObj AggregateRequestShapifier::_makeQueryStatsKeyHelper(
    const SerializationOptions& opts,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const Pipeline& pipeline) const {
    BSONObjBuilder bob;

    bob.append("queryShape", query_shape::extractQueryShape(_request, pipeline, opts, expCtx));

    // cursor
    if (auto param = _request.getCursor().getBatchSize()) {
        BSONObjBuilder cursorInfo = bob.subobjStart(AggregateCommandRequest::kCursorFieldName);
        opts.appendLiteral(&cursorInfo,
                           SimpleCursorOptions::kBatchSizeFieldName,
                           static_cast<long long>(param.get()));
        cursorInfo.done();
    }

    // maxTimeMS
    if (auto param = _request.getMaxTimeMS()) {
        opts.appendLiteral(&bob,
                           AggregateCommandRequest::kMaxTimeMSFieldName,
                           static_cast<long long>(param.get()));
    }

    // bypassDocumentValidation
    if (auto param = _request.getBypassDocumentValidation()) {
        opts.appendLiteral(
            &bob, AggregateCommandRequest::kBypassDocumentValidationFieldName, bool(param.get()));
    }

    // comment
    if (_comment) {
        opts.appendLiteral(&bob, "comment", *_comment);
    }

    // TODO SERVER-77190 include the whole client metadata
    // applicationName
    if (_applicationName.has_value()) {
        bob.append("applicationName", _applicationName.value());
    }

    return bob.obj();
}
}  // namespace mongo::query_stats
