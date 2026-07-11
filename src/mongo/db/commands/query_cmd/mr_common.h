// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/query_cmd/map_reduce_gen.h"
#include "mongo/db/commands/query_cmd/map_reduce_out_options.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::map_reduce_common {

struct OutputOptions {
    std::string outDB;
    std::string collectionName;
    NamespaceString finalNamespace;
    // if true, no lock during output operation
    bool outNonAtomic;
    OutputType outType;
};

OutputOptions parseOutputOptions(const DatabaseName& dbname, const BSONObj& cmdObj);

Status checkAuthForMapReduce(const BasicCommand* command,
                             OperationContext* opCtx,
                             const DatabaseName& dbName,
                             const BSONObj& cmdObj);

/**
 * Returns true if the provided mapReduce command has an 'out' parameter.
 */
bool mrSupportsWriteConcern(const BSONObj& cmd);

/**
 * Accepts a parsed mapReduce command and returns the equivalent aggregation pipeline. Note that the
 * returned pipeline does *not* contain a $cursor stage and thus is not runnable.
 */
std::unique_ptr<Pipeline> translateFromMR(MapReduceCommandRequest parsedMr,
                                          boost::intrusive_ptr<ExpressionContext> expCtx);

}  // namespace mongo::map_reduce_common
