// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/synchronized_value.h"

#include <string_view>

namespace mongo {

void QueryPlanRanker::append(OperationContext*, BSONObjBuilder* b, std::string_view name) {
    *b << name << idl::serialize(_data.get());
}

Status QueryPlanRanker::setFromString(std::string_view value) {
    _data =
        idl::deserialize<QueryPlanRankerEnum>(value, IDLParserContext("internalQueryPlanRanker"));
    return Status::OK();
}

void QueryCBRCEMode::append(OperationContext*, BSONObjBuilder* b, std::string_view name) {
    *b << name << idl::serialize(_data.get());
}

Status QueryCBRCEMode::setFromString(std::string_view value) {
    QueryCBRCEModeEnum mode =
        idl::deserialize<QueryCBRCEModeEnum>(value, IDLParserContext("internalQueryCBRCEMode"));
    if (mode == QueryCBRCEModeEnum::kHistogramCE && !getTestCommandsEnabled()) {
        return Status(ErrorCodes::BadValue, "histogramCE not allowed");
    }
    _data = mode;
    return Status::OK();
}

}  // namespace mongo
