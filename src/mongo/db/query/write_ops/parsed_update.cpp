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

#include "mongo/db/query/write_ops/parsed_update.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/matcher/expression_with_placeholder.h"
#include "mongo/db/matcher/extensions_callback.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_utils.h"
#include "mongo/db/query/write_ops/parsed_update_array_filters.h"
#include "mongo/db/query/write_ops/parsed_writes_common.h"
#include "mongo/db/query/write_ops/update_request.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/shard_role/shard_catalog/collection_operation_source.h"
#include "mongo/db/update/update_driver.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <map>
#include <memory>
#include <utility>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

PlanYieldPolicy::YieldPolicy getUpdateYieldPolicy(const UpdateRequest* request) {
    return request->isGod() ? PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY
                            : request->getYieldPolicy();
}


bool ParsedUpdate::hasParsedFindCommand() const {
    return parsedFind.get() != nullptr;
}

const UpdateRequest* ParsedUpdate::getRequest() const {
    return request;
}

UpdateDriver* ParsedUpdate::getDriver() {
    return driver.get();
}

const UpdateDriver* ParsedUpdate::getDriver() const {
    return driver.get();
}

namespace parsed_update_command {
namespace {

Status parseQueryToParsedFindCommand(boost::intrusive_ptr<ExpressionContext> expCtx,
                                     ParsedUpdate& parsedUpdate) {
    dassert(!parsedUpdate.parsedFind.get());

    auto swParsedFind = impl::parseWriteQueryToParsedFindCommand(
        expCtx.get(), *parsedUpdate.extensionsCallback, *parsedUpdate.request);

    if (swParsedFind.isOK()) {
        parsedUpdate.parsedFind = std::move(swParsedFind.getValue());
    }

    if (swParsedFind.getStatus().code() == ErrorCodes::QueryFeatureNotAllowed) {
        // The default error message for disallowed $expr is not descriptive enough, so we rewrite
        // it here.
        return {ErrorCodes::QueryFeatureNotAllowed,
                "$expr is not allowed in the query predicate for an upsert"};
    }

    return swParsedFind.getStatus();
}

/**
 * Parses the query portion of the update request.
 */
Status parseQuery(boost::intrusive_ptr<ExpressionContext> expCtx, ParsedUpdate& parsedUpdate) {
    dassert(!parsedUpdate.parsedFind.get());

    if (!parsedUpdate.driver->needMatchDetails() &&
        isSimpleIdQuery(parsedUpdate.request->getQuery())) {
        return Status::OK();
    }

    return parseQueryToParsedFindCommand(expCtx, parsedUpdate);
}

/**
 * Parses the update-descriptor portion of the update request.
 */
void parseUpdate(boost::intrusive_ptr<ExpressionContext> expCtx, ParsedUpdate& parsedUpdate) {
    parsedUpdate.driver->setCollator(expCtx->getCollator());
    parsedUpdate.driver->setLogOp(true);
    parsedUpdate.driver->setFromOplogApplication(parsedUpdate.request->isFromOplogApplication());

    auto source = parsedUpdate.request->source();
    if ((source == OperationSource::kFromMigrate) ||
        parsedUpdate.request->getBypassEmptyTsReplacement()) {
        parsedUpdate.driver->setBypassEmptyTsReplacement(true);
    }

    // Time-series operations will not result in any documents with dots or dollars fields.
    if (source == OperationSource::kTimeseriesInsert ||
        source == OperationSource::kTimeseriesUpdate) {
        parsedUpdate.driver->setSkipDotsDollarsCheck(true);
    }

    expCtx->setIsParsingPipelineUpdate(true);
    parsedUpdate.driver->parse(*parsedUpdate.modification,
                               *parsedUpdate.arrayFilters,
                               parsedUpdate.request->getUpdateConstants(),
                               parsedUpdate.request->isMulti());
    expCtx->setIsParsingPipelineUpdate(false);
}

/**
 * Parses the update request to a canonical query and an update driver. On success, the
 * parsed update can be used to create a PlanExecutor for this update.
 */
Status parseRequest(boost::intrusive_ptr<ExpressionContext> expCtx, ParsedUpdate& parsedUpdate) {
    // It is invalid to request that the UpdateStage return the prior or newly-updated version
    // of a document during a multi-update.
    tassert(11052005,
            "Cannot request UpdateStage to return the prior or newly-updated version of a document "
            "during a multi-update",
            !(parsedUpdate.request->shouldReturnAnyDocs() && parsedUpdate.request->isMulti()));

    // It is invalid to specify 'upsertSupplied:true' for a non-upsert operation, or if no upsert
    // document was supplied with the request.
    if (parsedUpdate.request->shouldUpsertSuppliedDocument()) {
        uassert(ErrorCodes::FailedToParse,
                str::stream() << "cannot specify '"
                              << write_ops::UpdateOpEntry::kUpsertSuppliedFieldName
                              << ": true' for a non-upsert operation",
                parsedUpdate.request->isUpsert());
        const auto& constants = parsedUpdate.request->getUpdateConstants();
        uassert(ErrorCodes::FailedToParse,
                str::stream() << "the parameter '"
                              << write_ops::UpdateOpEntry::kUpsertSuppliedFieldName
                              << "' is set to 'true', but no document was supplied",
                constants && (*constants)["new"_sd].type() == BSONType::object);
    }

    // It is invalid to request that a ProjectionStage be applied to the UpdateStage if the
    // UpdateStage would not return any document.
    tassert(
        11052006,
        "Cannot apply projection to UpdateStage if the UpdateStage would not return any document",
        parsedUpdate.request->getProj().isEmpty() || parsedUpdate.request->shouldReturnAnyDocs());

    auto statusWithArrayFilters =
        parsedUpdateArrayFilters(expCtx,
                                 parsedUpdate.request->getArrayFilters(),
                                 parsedUpdate.request->getNamespaceString());
    if (!statusWithArrayFilters.isOK()) {
        return statusWithArrayFilters.getStatus();
    }
    parsedUpdate.arrayFilters =
        std::make_unique<std::map<StringData, std::unique_ptr<ExpressionWithPlaceholder>>>(
            std::move(statusWithArrayFilters.getValue()));

    expCtx->startExpressionCounters();

    // We parse the update portion before the query portion because the dispostion of the update
    // may determine whether or not we need to produce a CanonicalQuery at all.  For example, if
    // the update involves the positional-dollar operator, we must have a CanonicalQuery even if
    // it isn't required for query execution.
    parseUpdate(expCtx, parsedUpdate);
    Status status = parseQuery(expCtx, parsedUpdate);

    expCtx->initializeReferencedSystemVariables();

    return status;
}
}  // namespace

StatusWith<ParsedUpdate> parse(boost::intrusive_ptr<ExpressionContext> expCtx,
                               const UpdateRequest* request,
                               std::unique_ptr<const ExtensionsCallback> extensionsCallback) {
    // Note: The caller should hold a lock on the 'collection' if it really exists so that it can
    // stay alive until the end of the ParsedUpdate's lifetime.
    ParsedUpdate out;
    out.request = request;
    out.driver = std::make_unique<UpdateDriver>(expCtx);
    out.modification =
        std::make_unique<write_ops::UpdateModification>(out.request->getUpdateModification());
    out.extensionsCallback = std::move(extensionsCallback);

    auto status = parseRequest(expCtx, out);
    if (status.isOK()) {
        return out;
    }
    return status;
}
}  // namespace parsed_update_command

}  // namespace mongo
