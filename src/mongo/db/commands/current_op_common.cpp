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

#include "mongo/platform/basic.h"

#include "mongo/db/commands/current_op_common.h"

#include <string>

#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/namespace_string.h"
#include "mongo/idl/command_generic_argument.h"

namespace mongo {
namespace {
static constexpr auto kAll = "$all"_sd;
static constexpr auto kOwnOps = "$ownOps"_sd;
static constexpr auto kTruncateOps = "$truncateOps"_sd;
static const StringDataSet kCurOpCmdParams = {kAll, kOwnOps, kTruncateOps};
}  // namespace

bool CurrentOpCommandBase::run(OperationContext* opCtx,
                               const DatabaseName& dbName,
                               const BSONObj& cmdObj,
                               BSONObjBuilder& result) {
    // Convert the currentOp command spec into an equivalent aggregation command. This will be
    // of the form {aggregate:1, pipeline: [{$currentOp: {idleConnections: $all, allUsers:
    // !$ownOps, truncateOps: true}}, {$match: {<filter>}}, {$group: {_id: null, inprog: {$push:
    // "$$ROOT"}}}], cursor:{}}
    std::vector<BSONObj> pipeline;

    // {$currentOp: {idleConnections: $all, allUsers: !$ownOps, truncateOps: true}}
    BSONObjBuilder currentOpBuilder;
    BSONObjBuilder currentOpSpecBuilder(currentOpBuilder.subobjStart("$currentOp"));

    // If test commands are enabled, then we allow the currentOp commands to specify whether or not
    // to truncate long operations via the '$truncateOps' parameter. Otherwise, we always truncate
    // operations to match the behaviour of the legacy currentOp command.
    const bool truncateOps =
        !getTestCommandsEnabled() || !cmdObj[kTruncateOps] || cmdObj[kTruncateOps].trueValue();

    currentOpSpecBuilder.append("idleConnections", cmdObj[kAll].trueValue());
    currentOpSpecBuilder.append("allUsers", !cmdObj[kOwnOps].trueValue());
    currentOpSpecBuilder.append("truncateOps", truncateOps);
    currentOpSpecBuilder.doneFast();

    pipeline.push_back(currentOpBuilder.obj());

    // {$match: {<user-defined filter>}}
    BSONObjBuilder matchBuilder;
    BSONObjBuilder matchSpecBuilder(matchBuilder.subobjStart("$match"));

    size_t idx = 0;
    for (const auto& elt : cmdObj) {
        const auto fieldName = elt.fieldNameStringData();

        if (0 == idx++ || kCurOpCmdParams.count(fieldName) || isGenericArgument(fieldName)) {
            continue;
        }

        matchSpecBuilder.append(elt);
    }

    matchSpecBuilder.doneFast();

    pipeline.push_back(matchBuilder.obj());

    // Perform any required modifications to the pipeline before adding the final $group stage.
    modifyPipeline(&pipeline);

    // {$group: {_id: null, inprog: {$push: "$$ROOT"}}}
    BSONObjBuilder groupBuilder;
    BSONObjBuilder groupSpecBuilder(groupBuilder.subobjStart("$group"));

    groupSpecBuilder.append("_id", 0);

    BSONObjBuilder inprogSpecBuilder(groupSpecBuilder.subobjStart("inprog"));

    inprogSpecBuilder.append("$push", "$$ROOT");

    inprogSpecBuilder.doneFast();
    groupSpecBuilder.doneFast();

    pipeline.push_back(groupBuilder.obj());

    // Pipeline is complete; create an AggregateCommandRequest for $currentOp.
    AggregateCommandRequest request(NamespaceString::makeCollectionlessAggregateNSS(
                                        DatabaseNameUtil::deserialize(dbName.tenantId(), "admin")),
                                    std::move(pipeline));

    // Run the pipeline and obtain a CursorResponse.
    auto aggResults = uassertStatusOK(runAggregation(opCtx, request));

    if (aggResults.getBatch().empty()) {
        result.append("inprog", BSONArray());
    } else {
        result.append(aggResults.getBatch().front()["inprog"]);
    }

    // Make any final custom additions to the response object.
    appendToResponse(&result);

    return true;
}

}  // namespace mongo
