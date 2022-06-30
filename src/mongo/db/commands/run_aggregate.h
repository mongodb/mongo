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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/rpc/op_msg_rpc_impls.h"

namespace mongo {

/**
 * Executes the aggregation 'request' over the specified namespace 'nss' using context 'opCtx'.
 *
 * The raw aggregate command parameters should be passed in 'cmdObj', and will be reported as the
 * originatingCommand in subsequent getMores on the resulting agg cursor.
 *
 * 'privileges' contains the privileges that were required to run this aggregation, to be used later
 * for re-checking privileges for getMore commands.
 *
 * On success, fills out 'result' with the command response.
 */
Status runAggregate(OperationContext* opCtx,
                    const NamespaceString& nss,
                    AggregateCommandRequest& request,
                    const LiteParsedPipeline& liteParsedPipeline,
                    const BSONObj& cmdObj,
                    const PrivilegeVector& privileges,
                    rpc::ReplyBuilderInterface* result);

/**
 * Convenience version that internally constructs the LiteParsedPipeline.
 */
Status runAggregate(OperationContext* opCtx,
                    const NamespaceString& nss,
                    AggregateCommandRequest& request,
                    const BSONObj& cmdObj,
                    const PrivilegeVector& privileges,
                    rpc::ReplyBuilderInterface* result);

/**
 * Tracks explicit use of allowDiskUse:false with find and aggregate commands.
 */
extern CounterMetric allowDiskUseFalseCounter;
}  // namespace mongo
