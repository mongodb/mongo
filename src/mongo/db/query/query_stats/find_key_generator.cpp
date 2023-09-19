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

#include "mongo/db/query/query_stats/find_key_generator.h"

namespace mongo::query_stats {

BSONObj FindCmdQueryStatsStoreKeyComponents::shapifyReadConcern(const BSONObj& readConcern,
                                                                const SerializationOptions& opts) {
    // Read concern should not be considered a literal.
    // afterClusterTime is distinct for every operation with causal consistency enabled. We
    // normalize it in order not to blow out the telemetry store cache.
    if (readConcern["afterClusterTime"].eoo()) {
        return readConcern.copy();
    } else {
        BSONObjBuilder bob;

        if (auto levelElem = readConcern["level"]) {
            bob.append(levelElem);
        }
        opts.appendLiteral(&bob, "afterClusterTime", readConcern["afterClusterTime"]);
        return bob.obj();
    }
}

void FindCmdQueryStatsStoreKeyComponents::appendTo(BSONObjBuilder& bob,
                                                   const SerializationOptions& opts) const {
    if (_hasField.readConcern) {
        auto readConcernToAppend = _shapifiedReadConcern;
        if (opts != SerializationOptions::kRepresentativeQueryShapeSerializeOptions) {
            // The options aren't the same as the first time we shapified, so re-computation is
            // necessary (e.g. use "?timestamp" instead of the representative Timestamp(0, 0)).
            readConcernToAppend = shapifyReadConcern(_shapifiedReadConcern, opts);
        }
        bob.append(FindCommandRequest::kReadConcernFieldName, readConcernToAppend);
    }

    if (_hasField.allowPartialResults) {
        bob.append(FindCommandRequest::kAllowPartialResultsFieldName, _allowPartialResults);
    }

    // Fields for literal redaction. Adds batchSize, maxTimeMS, and noCursorTimeOut.

    if (_hasField.noCursorTimeout) {
        // Capture whether noCursorTimeout was specified in the query, do not distinguish
        // between true or false.
        opts.appendLiteral(
            &bob, FindCommandRequest::kNoCursorTimeoutFieldName, _hasField.noCursorTimeout);
    }

    // We don't store the specified maxTimeMS or batch size values since they don't matter.
    // Provide an arbitrary literal long here.
    tassert(7973602,
            "Serialization policy not supported - original values have been discarded",
            opts.literalPolicy != LiteralSerializationPolicy::kUnchanged);
    if (_hasField.maxTimeMS) {
        opts.appendLiteral(&bob, FindCommandRequest::kMaxTimeMSFieldName, 0ll);
    }
    if (_hasField.batchSize) {
        opts.appendLiteral(&bob, FindCommandRequest::kBatchSizeFieldName, 0ll);
    }
}
std::unique_ptr<FindCommandRequest> FindKeyGenerator::reparse(OperationContext* opCtx) const {
    auto fcr =
        static_cast<const query_shape::FindCmdShape*>(universalComponents()._queryShape.get())
            ->toFindCommandRequest();
    if (_components._hasField.readConcern)
        fcr->setReadConcern(_components._shapifiedReadConcern);
    if (_components._hasField.allowPartialResults)
        fcr->setAllowPartialResults(_components._allowPartialResults);
    if (_components._hasField.batchSize)
        fcr->setBatchSize(1ll);
    if (_components._hasField.maxTimeMS)
        fcr->setMaxTimeMS(1ll);
    return fcr;
}
}  // namespace mongo::query_stats
