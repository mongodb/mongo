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

#include <boost/intrusive_ptr.hpp>
#include <boost/optional.hpp>
#include <cstdint>
#include <string>
#include <utility>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/map_reduce_agg.h"
#include "mongo/db/commands/map_reduce_javascript_code.h"
#include "mongo/db/commands/mr_common.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/pipeline_d.h"
#include "mongo/db/query/map_reduce_output_format.h"

namespace mongo::map_reduce_agg {

namespace {

auto makeExpressionContext(OperationContext* opCtx, const MapReduce& parsedMr) {
    // AutoGetCollectionForReadCommand will throw if the sharding version for this connection is
    // out of date.
    AutoGetCollectionForReadCommand ctx(
        opCtx, parsedMr.getNamespace(), AutoGetCollection::ViewMode::kViewsPermitted);
    uassert(ErrorCodes::CommandNotSupportedOnView,
            "mapReduce on a view is not yet supported",
            !ctx.getView());

    auto resolvedCollator = PipelineD::resolveCollator(
        opCtx, parsedMr.getCollation().get_value_or(BSONObj()), ctx.getCollection());

    // The UUID of the collection for the execution namespace of this aggregation.
    auto uuid =
        ctx.getCollection() ? boost::make_optional(ctx.getCollection()->uuid()) : boost::none;

    auto runtimeConstants = Variables::generateRuntimeConstants(opCtx);
    if (parsedMr.getScope()) {
        runtimeConstants.setJsScope(parsedMr.getScope()->getObj());
    }

    // Manually build an ExpressionContext with the desired options for the translated
    // aggregation. The one option worth noting here is allowDiskUse, which is required to allow
    // the $group stage of the translated pipeline to spill to disk.
    return make_intrusive<ExpressionContext>(
        opCtx,
        boost::none,  // explain
        false,        // fromMongos
        false,        // needsmerge
        true,         // allowDiskUse
        parsedMr.getBypassDocumentValidation().get_value_or(false),
        parsedMr.getNamespace(),
        parsedMr.getCollation().get_value_or(BSONObj()),
        runtimeConstants,
        std::move(resolvedCollator),
        MongoProcessInterface::create(opCtx),
        StringMap<ExpressionContext::ResolvedNamespace>{},  // resolvedNamespaces
        uuid);
}

}  // namespace

bool runAggregationMapReduce(OperationContext* opCtx,
                             const std::string& dbname,
                             const BSONObj& cmd,
                             std::string& errmsg,
                             BSONObjBuilder& result) {
    auto exhaustPipelineIntoBSONArray = [](auto&& pipeline) {
        BSONArrayBuilder bab;
        while (auto&& doc = pipeline->getNext())
            bab.append(doc->toBson());
        return bab.arr();
    };

    auto parsedMr = MapReduce::parse(IDLParserErrorContext("MapReduce"), cmd);
    auto expCtx = makeExpressionContext(opCtx, parsedMr);
    auto runnablePipeline = [&]() {
        auto pipeline = map_reduce_common::translateFromMR(parsedMr, expCtx);
        return expCtx->mongoProcessInterface->attachCursorSourceToPipelineForLocalRead(
            expCtx, pipeline.release());
    }();

    if (parsedMr.getOutOptions().getOutputType() == OutputType::InMemory) {
        map_reduce_output_format::appendInlineResponse(
            exhaustPipelineIntoBSONArray(runnablePipeline),
            boost::get_optional_value_or(parsedMr.getVerbose(), false),
            false,
            &result);
    } else {
        // For non-inline output, the pipeline should not return any results however getNext() still
        // needs to be called once to ensure documents are written to the output collection.
        invariant(!runnablePipeline->getNext());

        map_reduce_output_format::appendOutResponse(
            parsedMr.getOutOptions().getDatabaseName(),
            parsedMr.getOutOptions().getCollectionName(),
            boost::get_optional_value_or(parsedMr.getVerbose(), false),
            false,
            &result);
    }

    return true;
}

}  // namespace mongo::map_reduce_agg
