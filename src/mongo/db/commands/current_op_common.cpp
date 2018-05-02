/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/commands/current_op_common.h"

#include <string>

#include "mongo/db/command_generic_argument.h"
#include "mongo/db/namespace_string.h"

namespace mongo {

bool CurrentOpCommandBase::run(OperationContext* opCtx,
                               const std::string& dbName,
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

    currentOpSpecBuilder.append("idleConnections", cmdObj["$all"].trueValue());
    currentOpSpecBuilder.append("allUsers", !cmdObj["$ownOps"].trueValue());
    currentOpSpecBuilder.append("truncateOps", true);
    currentOpSpecBuilder.doneFast();

    pipeline.push_back(currentOpBuilder.obj());

    // {$match: {<user-defined filter>}}
    BSONObjBuilder matchBuilder;
    BSONObjBuilder matchSpecBuilder(matchBuilder.subobjStart("$match"));

    size_t idx = 0;
    for (const auto& elt : cmdObj) {
        const auto fieldName = elt.fieldNameStringData();

        if (0 == idx++ || fieldName == "$all" || fieldName == "$ownOps" ||
            isGenericArgument(fieldName)) {
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

    // Pipeline is complete; create an AggregationRequest for $currentOp.
    const AggregationRequest request(NamespaceString::makeCollectionlessAggregateNSS("admin"),
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
