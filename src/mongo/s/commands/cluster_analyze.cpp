/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/analyze_command_gen.h"
#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/chunk_manager_targeter.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/cluster_ddl.h"
#include "mongo/s/grid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

constexpr auto kRawFieldName = "raw"_sd;
constexpr auto kWriteConcernErrorFieldName = "writeConcernError"_sd;

class AnalyzeCmd : public BasicCommandWithRequestParser<AnalyzeCmd> {
public:
    using Request = AnalyzeCommandRequest;
    using Reply = AnalyzeCommandReply;

    bool adminOnly() const override {
        return false;
    }

    bool skipApiVersionCheck() const override {
        // Internal testing command
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const std::string& dbname,
                                 const BSONObj& cmdObj) const override {
        // TODO
        return Status::OK();
    }

    bool supportsWriteConcern(const BSONObj& cmd) const final {
        return true;
    }

    bool runWithRequestParser(OperationContext* opCtx,
                              const std::string& dbName,
                              const BSONObj& cmdObj,
                              const RequestParser& requestParser,
                              BSONObjBuilder& output) final {
        const NamespaceString nss(CommandHelpers::parseNsCollectionRequired(dbName, cmdObj));
        LOGV2_DEBUG(67655,
                    1,
                    "analyze: {namespace} cmd: {command}",
                    "CMD: analyze",
                    "namespace"_attr = nss,
                    "command"_attr = redact(cmdObj));

        auto routingInfo =
            uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));
        auto shardResponses = scatterGatherVersionedTargetByRoutingTable(
            opCtx,
            nss.db(),
            nss,
            routingInfo,
            applyReadWriteConcern(
                opCtx, this, CommandHelpers::filterCommandRequestForPassthrough(cmdObj)),
            ReadPreferenceSetting::get(opCtx),
            Shard::RetryPolicy::kIdempotent,
            BSONObj() /* query */,
            BSONObj() /* collation */);

        Status firstFailedShardStatus = Status::OK();
        bool isValid = true;

        BSONObjBuilder rawResBuilder(output.subobjStart("raw"));
        for (const auto& shardResponse : shardResponses) {
            const auto& swResponse = shardResponse.swResponse;
            if (!swResponse.isOK()) {
                rawResBuilder.append("Error: "_sd, BSON(swResponse.getStatus().toString()));
                firstFailedShardStatus = swResponse.getStatus();
                break;
            }
        }
        rawResBuilder.done();

        if (firstFailedShardStatus.isOK())
            output.appendBool("valid", isValid);

        uassertStatusOK(firstFailedShardStatus);
        return true;
    }

    void validateResult(const BSONObj& result) final {
        auto ctx = IDLParserContext("AnalyzeCommandReply");
        if (checkIsErrorStatus(result, ctx)) {
            return;
        }

        StringDataSet ignorableFields(
            {kWriteConcernErrorFieldName, ErrorReply::kOkFieldName, kRawFieldName});
        Reply::parse(ctx, result.removeFields(ignorableFields));
        if (!result.hasField(kRawFieldName)) {
            return;
        }

        const auto& rawData = result[kRawFieldName];
    }

    const AuthorizationContract* getAuthorizationContract() const final {
        return &::mongo::AnalyzeCommandRequest::kAuthorizationContract;
    }
} analyzeCmd;

}  // namespace
}  // namespace mongo
