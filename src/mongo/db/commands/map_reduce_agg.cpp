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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/map_reduce_agg.h"
#include "mongo/db/commands/map_reduce_gen.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/getmore_request.h"
#include "mongo/db/query/mr_response_formatter.h"

namespace mongo {

namespace {

std::unique_ptr<Pipeline, PipelineDeleter> translateFromMR(
    MapReduce parsedMr, boost::intrusive_ptr<ExpressionContext> expCtx) {
    return uassertStatusOK(Pipeline::create({}, expCtx));
}

boost::intrusive_ptr<ExpressionContext> makeExpressionContext() {
    return nullptr;
}

}  // namespace

// Update MapReduceFormatter
bool runAggregationMapReduce(OperationContext* opCtx,
                             const std::string& dbname,
                             const BSONObj& cmd,
                             std::string& errmsg,
                             BSONObjBuilder& result) {
    auto mrRequest = MapReduce::parse(IDLParserErrorContext("MapReduce"), cmd);
    [[maybe_unused]] bool inMemory =
        mrRequest.getOutOptions().getOutputType() == OutputType::InMemory;

    [[maybe_unused]] auto pipe = translateFromMR(mrRequest, makeExpressionContext());

    // MapReduceResponseFormatter(
    //     std::move(completeCursor),
    //     boost::make_optional(!inMemory, NamespaceString(std::move(outDb), std::move(outColl))),
    //     boost::get_optional_value_or(mrRequest.getVerbose(), false))
    //     .appendAsMapReduceResponse(&result);
    return true;
}

}  // namespace mongo
