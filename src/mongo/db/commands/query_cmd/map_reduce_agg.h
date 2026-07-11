// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/query_cmd/map_reduce_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>

#include <boost/optional/optional.hpp>

namespace mongo::map_reduce_agg {

/**
 * Executes a mapReduce command against a replica set/standalone.
 */
bool runAggregationMapReduce(OperationContext* opCtx,
                             const DatabaseName& dbName,
                             const BSONObj& cmd,
                             BSONObjBuilder& result,
                             boost::optional<ExplainOptions::Verbosity> verbosity);

}  // namespace mongo::map_reduce_agg
