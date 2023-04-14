/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <string>
#include <vector>

#include "mongo/db/commands.h"
#include "mongo/db/commands/map_reduce_gen.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/pipeline.h"

namespace mongo::map_reduce_common {

struct OutputOptions {
    std::string outDB;
    std::string collectionName;
    NamespaceString finalNamespace;
    // if true, no lock during output operation
    bool outNonAtomic;
    OutputType outType;
};

OutputOptions parseOutputOptions(StringData dbname, const BSONObj& cmdObj);

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
std::unique_ptr<Pipeline, PipelineDeleter> translateFromMR(
    MapReduceCommandRequest parsedMr, boost::intrusive_ptr<ExpressionContext> expCtx);

}  // namespace mongo::map_reduce_common
