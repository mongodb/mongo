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

#pragma once

#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/exchange_spec_gen.h"
#include "mongo/db/pipeline/legacy_runtime_constants_gen.h"
#include "mongo/db/pipeline/plan_executor_pipeline.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/serialization_context.h"

namespace mongo {

template <typename T>
class StatusWith;
class Document;
class AggregateCommandRequest;
class OperationContext;

namespace aggregation_request_helper {

/**
 * Helpers to serialize/deserialize AggregateCommandRequest.
 */
static constexpr StringData kBatchSizeField = "batchSize"_sd;
static constexpr long long kDefaultBatchSize = 101;

/**
 * Create a new instance of AggregateCommandRequest by parsing the raw command object. Throws an
 * exception if a required field was missing, if there was an unrecognized field name, or if there
 * was a bad value for one of the fields.
 *
 * If we are parsing a request for an explained aggregation with an explain verbosity provided,
 * then 'explainVerbosity' contains this information. In this case, 'cmdObj' may not itself
 * contain the explain specifier. Otherwise, 'explainVerbosity' should be boost::none.
 *
 * Callers must provide the validated tenancy scope (if any) to ensure that any namespaces
 * deserialized from the aggregation request properly account for the tenant ID.
 */
AggregateCommandRequest parseFromBSON(
    const BSONObj& cmdObj,
    const boost::optional<auth::ValidatedTenancyScope>& vts,
    boost::optional<ExplainOptions::Verbosity> explainVerbosity,
    const SerializationContext& serializationContext = SerializationContext());

StatusWith<AggregateCommandRequest> parseFromBSONForTests(
    const BSONObj& cmdObj,
    const boost::optional<auth::ValidatedTenancyScope>& vts = boost::none,
    boost::optional<ExplainOptions::Verbosity> explainVerbosity = boost::none);

/**
 * Serializes the options to a Document. Note that this serialization includes the original
 * pipeline object, as specified. Callers will likely want to override this field with a
 * serialization of a parsed and optimized Pipeline object.
 *
 * The explain option is not serialized. The preferred way to send an explain is with the explain
 * command, like: {explain: {aggregate: ...}, ...}, explain options are not part of the aggregate
 * command object.
 *
 * In case query settings are attached to 'expCtx', they will be serialized to the command document.
 */
Document serializeToCommandDoc(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                               const AggregateCommandRequest& request);

BSONObj serializeToCommandObj(const AggregateCommandRequest& request);

/**
 * Validates if 'AggregateCommandRequest' specs complies with API versioning. Throws uassert in case
 * of any failure.
 */
void validateRequestForAPIVersion(const OperationContext* opCtx,
                                  const AggregateCommandRequest& request);
/**
 * Validates if 'AggregateCommandRequest' sets the "isClusterQueryWithoutShardKeyCmd" field then the
 * request must have been fromRouter.
 */
void validateRequestFromClusterQueryWithoutShardKey(const AggregateCommandRequest& request);

/**
 * Returns the type of resumable scan required by this aggregation, if applicable. Otherwise returns
 * ResumableScanType::kNone.
 */
PlanExecutorPipeline::ResumableScanType getResumableScanType(const AggregateCommandRequest& request,
                                                             bool isChangeStream);

// TODO SERVER-95358 remove once 9.0 becomes last LTS.
const mongo::OptionalBool& getFromRouter(const AggregateCommandRequest& request);

// TODO SERVER-95358 remove once 9.0 becomes last LTS.
void setFromRouter(AggregateCommandRequest& request, mongo::OptionalBool value);

// TODO SERVER-95358 remove once 9.0 becomes last LTS.
void setFromRouter(MutableDocument& doc, mongo::Value value);
}  // namespace aggregation_request_helper

/**
 * Custom serializers/deserializers for AggregateCommandRequest.
 *
 * IMPORTANT: The method should not be modified, as API version input/output guarantees could
 * break because of it.
 */
boost::optional<mongo::ExplainOptions::Verbosity> parseExplainModeFromBSON(
    const BSONElement& explainElem);

/**
 * IMPORTANT: The method should not be modified, as API version input/output guarantees could
 * break because of it.
 */
void serializeExplainToBSON(const mongo::ExplainOptions::Verbosity& explain,
                            StringData fieldName,
                            BSONObjBuilder* builder);

/**
 * IMPORTANT: The method should not be modified, as API version input/output guarantees could
 * break because of it.
 */
mongo::SimpleCursorOptions parseAggregateCursorFromBSON(const BSONElement& cursorElem);

/**
 * IMPORTANT: The method should not be modified, as API version input/output guarantees could
 * break because of it.
 */
void serializeAggregateCursorToBSON(const SimpleCursorOptions& cursor,
                                    StringData fieldName,
                                    BSONObjBuilder* builder);

/**
 * Parse an aggregation pipeline definition from 'pipelineElem'.
 *
 * IMPORTANT: The method should not be modified, as API version input/output guarantees could
 * break because of it.
 */
static std::vector<BSONObj> parsePipelineFromBSON(const BSONElement& pipelineElem) {
    std::vector<BSONObj> pipeline;

    uassert(ErrorCodes::TypeMismatch,
            "'pipeline' option must be specified as an array",
            !pipelineElem.eoo() && pipelineElem.type() == BSONType::Array);

    for (auto elem : pipelineElem.Obj()) {
        uassert(ErrorCodes::TypeMismatch,
                "Each element of the 'pipeline' array must be an object",
                elem.type() == BSONType::Object);
        pipeline.push_back(elem.embeddedObject().getOwned());
    }

    return pipeline;
}
}  // namespace mongo
