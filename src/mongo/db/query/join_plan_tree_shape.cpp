// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_join_optimization_knobs_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/synchronized_value.h"

#include <string_view>

namespace mongo {

void JoinPlanTreeShape::append(OperationContext*, BSONObjBuilder* b, std::string_view name) {
    *b << name << idl::serialize(_data.get());
}

Status JoinPlanTreeShape::setFromString(std::string_view value) {
    _data = idl::deserialize<JoinPlanTreeShapeEnum>(value, IDLParserContext("JoinPlanTreeShape"));
    return Status::OK();
}

}  // namespace mongo
