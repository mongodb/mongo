// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/db/commands/query_cmd/current_op_common.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/database_name_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/idl/command_generic_argument.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/serialization_context.h"
#include "mongo/util/string_map.h"

#include <cstddef>
#include <utility>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;
static constexpr auto kAll = "$all"sv;
static constexpr auto kOwnOps = "$ownOps"sv;
static constexpr auto kTruncateOps = "$truncateOps"sv;
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
    const auto vts = auth::ValidatedTenancyScope::get(opCtx);
    const auto sc = vts != boost::none
        ? SerializationContext::stateCommandRequest(vts->hasTenantId(), vts->isFromAtlasProxy())
        : SerializationContext::stateCommandRequest();
    AggregateCommandRequest request(
        NamespaceString::makeCollectionlessAggregateNSS(DatabaseNameUtil::deserialize(
            dbName.tenantId(), "admin", SerializationContext::stateDefault())),
        std::move(pipeline),
        sc);

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
