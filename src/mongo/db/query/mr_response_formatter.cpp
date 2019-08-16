/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/query/mr_response_formatter.h"

namespace mongo {

MapReduceResponseFormatter::MapReduceResponseFormatter(CursorResponse aggregationResponse,
                                                       boost::optional<NamespaceString> outCollNss,
                                                       bool verbose)
    : _response(std::move(aggregationResponse)),
      _outCollNss(std::move(outCollNss)),
      _verbose(verbose) {}

void MapReduceResponseFormatter::appendResultsField(BSONObjBuilder* resultBuilder) {
    if (_outCollNss) {
        if (_outCollNss->db().empty()) {
            resultBuilder->append(MapReduceResponseFormatter::kResultField, _outCollNss->coll());
        } else {
            resultBuilder->append(
                MapReduceResponseFormatter::kResultField,
                BSON(kDbField << _outCollNss->db() << kCollectionField << _outCollNss->coll()));
        }
    } else {
        BSONArrayBuilder docsBuilder;
        auto batch = _response.releaseBatch();
        size_t sizeSoFar = 0;
        for (auto& doc : batch) {
            sizeSoFar += doc.objsize();
            docsBuilder.append(doc);
        }
        uassert(ErrorCodes::BSONObjectTooLarge,
                "too much data for in memory map/reduce",
                sizeSoFar < BSONObjMaxUserSize);
        resultBuilder->appendArray(MapReduceResponseFormatter::kResultsField, docsBuilder.arr());
    }
}

void MapReduceResponseFormatter::appendAsMapReduceResponse(BSONObjBuilder* resultBuilder) {
    appendResultsField(resultBuilder);
    // TODO: SERVER-42644 Build stats.
    resultBuilder->append(kTimeMillisField, 0);
    if (_verbose) {
        resultBuilder->append(kTimingField,
                              BSON(kMapTimeField << 0 << kEmitLoopField << 0 << kReduceTimeField
                                                 << 0 << kTotalField << 0));
    }

    resultBuilder->append(
        kCountsfield,
        BSON(kInputField << 0 << kEmitField << 0 << kReduceField << 0 << kOutputField << 0));
    resultBuilder->append(kOkField, 1);
}

void MapReduceResponseFormatter::appendAsClusterMapReduceResponse(BSONObjBuilder* resultBuilder) {
    appendResultsField(resultBuilder);
    // TODO: SERVER-42644 Build stats.
    resultBuilder->append(
        kCountsfield,
        BSON(kInputField << 0 << kEmitField << 0 << kReduceField << 0 << kOutputField << 0));
    resultBuilder->append(kTimeMillisField, 0);

    if (_verbose) {
        resultBuilder->append(kTimingField,
                              BSON(kShardProcessingField << 0 << kPostProcessing << 0));
    }

    resultBuilder->append(
        kShardCountsfield,
        BSON("shard-conn-string" << BSON(kInputField << 0 << kEmitField << 0 << kReduceField << 0
                                                     << kOutputField << 0)));
    resultBuilder->append(kPostProcessCountsField,
                          BSON("merging-shard-conn-string" << BSON(
                                   kInputField << 0 << kReduceField << 0 << kOutputField << 0)));
    resultBuilder->append(kOkField, 1);
}
}  // namespace mongo
