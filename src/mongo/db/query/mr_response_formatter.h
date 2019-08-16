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

#pragma once

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/cursor_response.h"

namespace mongo {

class MapReduceResponseFormatter {
public:
    static constexpr StringData kResultField = "result"_sd;
    static constexpr StringData kResultsField = "results"_sd;
    static constexpr StringData kDbField = "db"_sd;
    static constexpr StringData kCollectionField = "collection"_sd;
    static constexpr StringData kTimeMillisField = "timeMillis"_sd;
    static constexpr StringData kTimingField = "timing"_sd;
    static constexpr StringData kMapTimeField = "mapTime"_sd;
    static constexpr StringData kEmitLoopField = "emitLoop"_sd;
    static constexpr StringData kReduceTimeField = "reduceTime"_sd;
    static constexpr StringData kTotalField = "total"_sd;
    static constexpr StringData kShardProcessingField = "shardProcessing"_sd;
    static constexpr StringData kPostProcessing = "postProcessing"_sd;
    static constexpr StringData kCountsfield = "counts"_sd;
    static constexpr StringData kShardCountsfield = "shardCounts"_sd;
    static constexpr StringData kInputField = "input"_sd;
    static constexpr StringData kEmitField = "emit"_sd;
    static constexpr StringData kReduceField = "reduce"_sd;
    static constexpr StringData kOutputField = "output"_sd;
    static constexpr StringData kPostProcessCountsField = "postProcessCounts"_sd;
    static constexpr StringData kOkField = "ok"_sd;

    MapReduceResponseFormatter(CursorResponse aggregationResponse,
                               boost::optional<NamespaceString> outCollNss,
                               bool verbose);

    /*
     * Appends fields to 'resultBuilder' as if '_response' were a response from the mapReduce
     * command.
     */
    void appendAsMapReduceResponse(BSONObjBuilder* resultBuilder);

    /**
     * Appends fields to 'resultBuilder' as if '_response' were a response from the cluster
     * mapReduce command.
     */
    void appendAsClusterMapReduceResponse(BSONObjBuilder* resultBuilder);

private:
    CursorResponse _response;
    boost::optional<NamespaceString> _outCollNss;
    bool _verbose;

    /**
     * Appends the 'result(s)' field to the 'resultBuilder'.
     */
    void appendResultsField(BSONObjBuilder* resultBuilder);
};
}  // namespace mongo
