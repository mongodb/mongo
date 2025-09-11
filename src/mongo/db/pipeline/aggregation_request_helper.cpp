/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/pipeline/aggregation_request_helper.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/auth/validated_tenancy_scope_factory.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/client.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/server_options.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/decorable.h"
#include "mongo/util/str.h"

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace aggregation_request_helper {

/**
 * Validate the aggregate command object.
 */
void validate(const AggregateCommandRequest& aggregate,
              const BSONObj& cmdObj,
              const NamespaceString& nss,
              boost::optional<ExplainOptions::Verbosity> explainVerbosity);

StatusWith<AggregateCommandRequest> parseFromBSONForTests(
    const BSONObj& cmdObj,
    const boost::optional<auth::ValidatedTenancyScope>& vts,
    boost::optional<ExplainOptions::Verbosity> explainVerbosity) {
    try {
        return parseFromBSON(cmdObj, vts, explainVerbosity, SerializationContext::stateDefault());
    } catch (const AssertionException&) {
        return exceptionToStatus();
    }
}


AggregateCommandRequest parseFromBSON(const BSONObj& cmdObj,
                                      const boost::optional<auth::ValidatedTenancyScope>& vts,
                                      boost::optional<ExplainOptions::Verbosity> explainVerbosity,
                                      const SerializationContext& serializationContext) {
    auto tenantId = vts.has_value() ? boost::make_optional(vts->tenantId()) : boost::none;
    auto request = idl::parseCommandDocument<AggregateCommandRequest>(
        cmdObj, IDLParserContext("aggregate", vts, std::move(tenantId), serializationContext));
    if (explainVerbosity) {
        uassert(ErrorCodes::FailedToParse,
                str::stream() << "The '" << AggregateCommandRequest::kExplainFieldName
                              << "' option is illegal when a explain verbosity is also provided",
                !cmdObj.hasField(AggregateCommandRequest::kExplainFieldName));
        // The explain field on the aggregate request is a boolean with default value boost::none,
        // so we set it to true if expainVerbosity is defined.
        request.setExplain(true);
    }

    validate(request, cmdObj, request.getNamespace(), explainVerbosity);

    return request;
}

void addQuerySettingsToRequest(AggregateCommandRequest& request,
                               const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    const auto& querySettings = expCtx->getQuerySettings();
    if (!querySettings.toBSON().isEmpty()) {
        request.setQuerySettings(querySettings);
    }
}

void validate(const AggregateCommandRequest& aggregate,
              const BSONObj& cmdObj,
              const NamespaceString& nss,
              boost::optional<ExplainOptions::Verbosity> explainVerbosity) {
    bool hasExplainElem = aggregate.getExplain().has_value();
    bool hasExplain = explainVerbosity.has_value() || aggregate.getExplain().has_value();
    bool hasFromRouterElem = getFromRouter(aggregate).has_value();
    bool hasNeedsMergeElem = aggregate.getNeedsMerge().has_value();

    uassert(ErrorCodes::InvalidNamespace,
            fmt::format("Invalid collection name specified '{}'",
                        cmdObj.firstElement().valueStringDataSafe()),
            cmdObj.firstElement().valueStringDataSafe() !=
                NamespaceString::kCollectionlessAggregateCollection);

    // 'hasExplainElem' implies an aggregate command-level explain option, which does not require
    // a cursor argument.
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "The '" << AggregateCommandRequest::kCursorFieldName
                          << "' option is required, except for aggregate with the explain argument",
            hasExplainElem || cmdObj.hasField(AggregateCommandRequest::kCursorFieldName));

    uassert(ErrorCodes::FailedToParse,
            str::stream() << "Aggregation explain does not support the'"
                          << WriteConcernOptions::kWriteConcernField << "' option",
            !hasExplain || !aggregate.getWriteConcern().has_value());

    uassert(ErrorCodes::FailedToParse,
            str::stream() << "Cannot specify '" << AggregateCommandRequest::kNeedsMergeFieldName
                          << "' without '" << AggregateCommandRequest::kFromRouterFieldName << "'",
            (!hasNeedsMergeElem || hasFromRouterElem));

    uassert(ErrorCodes::FailedToParse,
            str::stream() << AggregateCommandRequest::kRequestReshardingResumeTokenFieldName
                          << " must only be set for the oplog namespace, not "
                          << nss.toStringForErrorMsg(),
            !aggregate.getRequestReshardingResumeToken().value_or(false) || nss.isOplog());

    uassert(ErrorCodes::FailedToParse,
            "Use of forcedPlanSolutionHash not permitted.",
            !aggregate.getForcedPlanSolutionHash() || internalQueryAllowForcedPlanByHash.load());

    bool hasRequestResumeToken = aggregate.getRequestResumeToken().value_or(false);
    uassert(ErrorCodes::FailedToParse,
            str::stream() << AggregateCommandRequest::kRequestResumeTokenFieldName
                          << " must be set for non-oplog namespace",
            !hasRequestResumeToken || !nss.isOplog());
    if (hasRequestResumeToken) {
        const auto& hintElem = aggregate.getHint();
        uassert(ErrorCodes::BadValue,
                "hint must be {$natural:1} if 'requestResumeToken' is enabled",
                hintElem.has_value() &&
                    SimpleBSONObjComparator::kInstance.evaluate(
                        hintElem.value() == BSON(query_request_helper::kNaturalSortField << 1)));
    }
}

void validateRequestForAPIVersion(const OperationContext* opCtx,
                                  const AggregateCommandRequest& request) {
    invariant(opCtx);

    auto apiParameters = APIParameters::get(opCtx);
    bool apiStrict = apiParameters.getAPIStrict().value_or(false);
    const auto apiVersion = apiParameters.getAPIVersion().value_or("");
    auto client = opCtx->getClient();

    // An internal client could be one of the following :
    //     - Does not have any transport session
    //     - The transport session tag is internal
    bool isInternalThreadOrClient = !client->session() || client->isInternalClient();
    // Checks that the 'exchange' or 'fromRouter' option can only be specified by the internal
    // client.
    if ((request.getExchange() || getFromRouter(request)) && apiStrict && apiVersion == "1") {
        uassert(ErrorCodes::APIStrictError,
                str::stream() << "'exchange' and 'fromRouter' option cannot be specified with "
                                 "'apiStrict: true' in API Version "
                              << apiVersion,
                isInternalThreadOrClient);
    }
}

void validateRequestFromClusterQueryWithoutShardKey(const AggregateCommandRequest& request) {
    if (request.getIsClusterQueryWithoutShardKeyCmd()) {
        uassert(ErrorCodes::InvalidOptions,
                "Only router can set the isClusterQueryWithoutShardKeyCmd field",
                getFromRouter(request));
    }
}

const mongo::OptionalBool& getFromRouter(const AggregateCommandRequest& request) {
    // Check both fields because we cannot rely on the feature flag checks. An aggregate command
    // with 'fromRouter' field set could be sent directly to an initial sync node with uninitialized
    // FCV, and creating/parsing/validating this command invocation happens before any check that
    // the node is a primary.
    if (const auto& fromRouter = request.getFromRouter(); fromRouter.has_value()) {
        return fromRouter;
    }
    return request.getFromMongos();
}

void setFromRouter(const VersionContext& vCtx,
                   AggregateCommandRequest& request,
                   mongo::OptionalBool value) {
    if (feature_flags::gFeatureFlagAggMongosToRouter.isEnabled(
            vCtx, serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        request.setFromRouter(value);
    } else {
        request.setFromMongos(value);
    }
}

void setFromRouter(const VersionContext& vCtx, MutableDocument& doc, mongo::Value value) {
    if (feature_flags::gFeatureFlagAggMongosToRouter.isEnabled(
            vCtx, serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {
        doc[AggregateCommandRequest::kFromRouterFieldName] = value;
    } else {
        doc[AggregateCommandRequest::kFromMongosFieldName] = value;
    }
}
}  // namespace aggregation_request_helper

// Custom serializers/deserializers for AggregateCommandRequest.

/**
 * IMPORTANT: The method should not be modified, as API version input/output guarantees could
 * break because of it.
 */
boost::optional<bool> parseExplainModeFromBSON(const BSONElement& explainElem) {
    uassert(ErrorCodes::TypeMismatch,
            "explain must be a boolean",
            explainElem.type() == BSONType::boolean);
    if (explainElem.Bool()) {
        return true;
    } else {
        return boost::none;
    }
}

/**
 * IMPORTANT: The method should not be modified, as API version input/output guarantees could
 * break because of it.
 */
void serializeExplainToBSON(const bool& explain, StringData fieldName, BSONObjBuilder* builder) {
    // Note that we do not serialize 'explain' field to the command object. This serializer only
    // serializes an empty cursor object for field 'cursor' when it is an explain command.
    builder->append(AggregateCommandRequest::kCursorFieldName, BSONObj());

    return;
}

/**
 * IMPORTANT: The method should not be modified, as API version input/output guarantees could
 * break because of it.
 */
mongo::SimpleCursorOptions parseAggregateCursorFromBSON(const BSONElement& cursorElem) {
    if (cursorElem.eoo()) {
        SimpleCursorOptions cursor;
        cursor.setBatchSize(aggregation_request_helper::kDefaultBatchSize);
        return cursor;
    }

    uassert(ErrorCodes::TypeMismatch,
            "cursor field must be missing or an object",
            cursorElem.type() == BSONType::object);

    SimpleCursorOptions cursor = SimpleCursorOptions::parse(
        cursorElem.embeddedObject(), IDLParserContext(AggregateCommandRequest::kCursorFieldName));
    if (!cursor.getBatchSize())
        cursor.setBatchSize(aggregation_request_helper::kDefaultBatchSize);

    return cursor;
}

/**
 * IMPORTANT: The method should not be modified, as API version input/output guarantees could
 * break because of it.
 */
void serializeAggregateCursorToBSON(const mongo::SimpleCursorOptions& cursor,
                                    StringData fieldName,
                                    BSONObjBuilder* builder) {
    if (!builder->hasField(fieldName)) {
        builder->append(
            fieldName,
            BSON(aggregation_request_helper::kBatchSizeField
                 << cursor.getBatchSize().value_or(aggregation_request_helper::kDefaultBatchSize)));
    }

    return;
}
}  // namespace mongo
