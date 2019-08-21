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
#include "mongo/db/db_raii.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_d.h"
#include "mongo/db/query/mr_response_formatter.h"
#include "mongo/util/intrusive_counter.h"

namespace mongo {

namespace {

std::unique_ptr<Pipeline, PipelineDeleter> translateFromMR(
    MapReduce parsedMr, boost::intrusive_ptr<ExpressionContext> expCtx) {
    return uassertStatusOK(Pipeline::create({}, expCtx));
}

auto makeExpressionContext(OperationContext* opCtx, const MapReduce& parsedMr) {
    // AutoGetCollectionForReadCommand will throw if the sharding version for this connection is out
    // of date.
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

    // Manually build an ExpressionContext with the desired options for the translated aggregation.
    // The one option worth noting here is allowDiskUse, which is required to allow the $group stage
    // of the translated pipeline to spill to disk.
    return make_intrusive<ExpressionContext>(
        opCtx,
        boost::none,    // explain
        std::string{},  // comment
        false,          // fromMongos
        false,          // needsmerge
        true,           // allowDiskUse
        parsedMr.getBypassDocumentValidation().get_value_or(false),
        parsedMr.getNamespace(),
        parsedMr.getCollation().get_value_or(BSONObj()),
        boost::none,  // runtimeConstants
        std::move(resolvedCollator),
        MongoProcessInterface::create(opCtx),
        StringMap<ExpressionContext::ResolvedNamespace>{},  // resolvedNamespaces
        uuid);
}

}  // namespace

// Update MapReduceFormatter
bool runAggregationMapReduce(OperationContext* opCtx,
                             const std::string& dbname,
                             const BSONObj& cmd,
                             std::string& errmsg,
                             BSONObjBuilder& result) {
    auto parsedMr = MapReduce::parse(IDLParserErrorContext("MapReduce"), cmd);
    [[maybe_unused]] auto pipe = translateFromMR(parsedMr, makeExpressionContext(opCtx, parsedMr));
    return true;
}

}  // namespace mongo
