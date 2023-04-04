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

#pragma once

#include <vector>

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/query/count_command_as_aggregation_command.h"
#include "mongo/db/query/count_command_gen.h"
#include "mongo/db/query/view_response_formatter.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/commands/cluster_explain.h"
#include "mongo/s/commands/strategy.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_aggregate.h"
#include "mongo/util/timer.h"

namespace mongo {

/**
 * Implements the find command on mongos.
 */
template <typename Impl>
class ClusterCountCmdBase final : public ErrmsgCommandDeprecated {
public:
    ClusterCountCmdBase() : ErrmsgCommandDeprecated(Impl::kName) {}

    const std::set<std::string>& apiVersions() const {
        return Impl::getApiVersions();
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    bool adminOnly() const override {
        return false;
    }

    ReadWriteType getReadWriteType() const override {
        return ReadWriteType::kRead;
    }

    bool supportsWriteConcern(const BSONObj& cmd) const override {
        return false;
    }

    ReadConcernSupportResult supportsReadConcern(const BSONObj& cmdObj,
                                                 repl::ReadConcernLevel level,
                                                 bool isImplicitDefault) const override {
        static const Status kSnapshotNotSupported{ErrorCodes::InvalidOptions,
                                                  "read concern snapshot not supported"};
        return {{level == repl::ReadConcernLevel::kSnapshotReadConcern, kSnapshotNotSupported},
                Status::OK()};
    }

    Status checkAuthForOperation(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const BSONObj& cmdObj) const override {
        auto* as = AuthorizationSession::get(opCtx->getClient());
        if (!as->isAuthorizedForActionsOnResource(parseResourcePattern(dbName, cmdObj),
                                                  ActionType::find)) {
            return {ErrorCodes::Unauthorized, "unauthorized"};
        }

        return Impl::checkAuthForOperation(opCtx);
    }

    bool errmsgRun(OperationContext* opCtx,
                   const std::string& dbname,
                   const BSONObj& cmdObj,
                   std::string& errmsg,
                   BSONObjBuilder& result) override {
        Impl::checkCanRunHere(opCtx);

        CommandHelpers::handleMarkKillOnClientDisconnect(opCtx);
        const NamespaceString nss(parseNs({boost::none, dbname}, cmdObj));
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Invalid namespace specified '" << nss.ns() << "'",
                nss.isValid());

        std::vector<AsyncRequestsSender::Response> shardResponses;
        try {
            auto countRequest = CountCommandRequest::parse(IDLParserContext("count"), cmdObj);

            if (shouldDoFLERewrite(countRequest)) {

                if (!countRequest.getEncryptionInformation()->getCrudProcessed().value_or(false)) {
                    processFLECountS(opCtx, nss, &countRequest);
                }

                CurOp::get(opCtx)->debug().shouldOmitDiagnosticInformation = true;
            }

            // We only need to factor in the skip value when sending to the shards if we
            // have a value for limit, otherwise, we apply it only once we have collected all
            // counts.
            if (countRequest.getLimit() && countRequest.getSkip()) {
                const auto limit = countRequest.getLimit().value();
                if (limit != 0) {
                    countRequest.setLimit(limit + countRequest.getSkip().value());
                }
            }
            countRequest.setSkip(boost::none);
            const auto cri = uassertStatusOK(
                Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));
            const auto collation = countRequest.getCollation().get_value_or(BSONObj());
            shardResponses = scatterGatherVersionedTargetByRoutingTable(
                opCtx,
                nss.db(),
                nss,
                cri,
                applyReadWriteConcern(
                    opCtx,
                    this,
                    countRequest.toBSON(
                        CommandHelpers::filterCommandRequestForPassthrough(cmdObj))),
                ReadPreferenceSetting::get(opCtx),
                Shard::RetryPolicy::kIdempotent,
                countRequest.getQuery(),
                collation,
                true /* eligibleForSampling */);
        } catch (const ExceptionFor<ErrorCodes::CommandOnShardedViewNotSupportedOnMongod>& ex) {
            // Rewrite the count command as an aggregation.
            auto countRequest = CountCommandRequest::parse(IDLParserContext("count"), cmdObj);
            auto aggCmdOnView =
                uassertStatusOK(countCommandAsAggregationCommand(countRequest, nss));
            auto aggCmdOnViewObj = OpMsgRequest::fromDBAndBody(nss.db(), aggCmdOnView).body;
            auto aggRequestOnView = aggregation_request_helper::parseFromBSON(
                opCtx,
                nss,
                aggCmdOnViewObj,
                boost::none,
                APIParameters::get(opCtx).getAPIStrict().value_or(false));

            auto resolvedAggRequest = ex->asExpandedViewAggregation(aggRequestOnView);
            auto resolvedAggCmd =
                aggregation_request_helper::serializeToCommandObj(resolvedAggRequest);

            BSONObj aggResult = CommandHelpers::runCommandDirectly(
                opCtx, OpMsgRequest::fromDBAndBody(dbname, std::move(resolvedAggCmd)));

            result.resetToEmpty();
            ViewResponseFormatter formatter(aggResult);
            auto formatStatus = formatter.appendAsCountResponse(&result, boost::none);
            uassertStatusOK(formatStatus);

            return true;
        } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
            // If there's no collection with this name, the count aggregation behavior below
            // will produce a total count of 0.
            shardResponses = {};
        }

        long long total = 0;
        BSONObjBuilder shardSubTotal(result.subobjStart("shards"));

        for (const auto& response : shardResponses) {
            auto status = response.swResponse.getStatus();
            if (status.isOK()) {
                status = getStatusFromCommandResult(response.swResponse.getValue().data);
                if (status.isOK()) {
                    long long shardCount = response.swResponse.getValue().data["n"].numberLong();
                    shardSubTotal.appendNumber(response.shardId.toString(), shardCount);
                    total += shardCount;
                    continue;
                }
            }

            shardSubTotal.doneFast();
            // Add error context so that you can see on which shard failed as well as details
            // about that error.
            uassertStatusOK(status.withContext(str::stream() << "failed on: " << response.shardId));
        }

        shardSubTotal.doneFast();
        total = applySkipLimit(total, cmdObj);
        result.appendNumber("n", total);
        return true;
    }

    Status explain(OperationContext* opCtx,
                   const OpMsgRequest& request,
                   ExplainOptions::Verbosity verbosity,
                   rpc::ReplyBuilderInterface* result) const override {
        Impl::checkCanExplainHere(opCtx);

        const BSONObj& cmdObj = request.body;

        CountCommandRequest countRequest(NamespaceStringOrUUID(NamespaceString{}));
        try {
            countRequest = CountCommandRequest::parse(IDLParserContext("count"), request);
        } catch (...) {
            return exceptionToStatus();
        }

        const NamespaceString nss(parseNs(request.getDatabase(), cmdObj));
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Invalid namespace specified '" << nss.ns() << "'",
                nss.isValid());

        // If the command has encryptionInformation, rewrite the query as necessary.
        if (shouldDoFLERewrite(countRequest)) {
            processFLECountS(opCtx, nss, &countRequest);

            CurOp::get(opCtx)->debug().shouldOmitDiagnosticInformation = true;
        }

        BSONObj targetingQuery = countRequest.getQuery();
        BSONObj targetingCollation = countRequest.getCollation().value_or(BSONObj());

        const auto explainCmd = ClusterExplain::wrapAsExplain(countRequest.toBSON({}), verbosity);

        // We will time how long it takes to run the commands on the shards
        Timer timer;

        std::vector<AsyncRequestsSender::Response> shardResponses;
        try {
            const auto cri = uassertStatusOK(
                Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));
            shardResponses =
                scatterGatherVersionedTargetByRoutingTable(opCtx,
                                                           nss.db(),
                                                           nss,
                                                           cri,
                                                           explainCmd,
                                                           ReadPreferenceSetting::get(opCtx),
                                                           Shard::RetryPolicy::kIdempotent,
                                                           targetingQuery,
                                                           targetingCollation);
        } catch (const ExceptionFor<ErrorCodes::CommandOnShardedViewNotSupportedOnMongod>& ex) {
            CountCommandRequest countRequest(NamespaceStringOrUUID(NamespaceString{}));
            try {
                countRequest = CountCommandRequest::parse(IDLParserContext("count"), cmdObj);
            } catch (...) {
                return exceptionToStatus();
            }

            auto aggCmdOnView = countCommandAsAggregationCommand(countRequest, nss);
            if (!aggCmdOnView.isOK()) {
                return aggCmdOnView.getStatus();
            }

            auto aggCmdOnViewObj =
                OpMsgRequest::fromDBAndBody(nss.db(), aggCmdOnView.getValue()).body;
            auto aggRequestOnView = aggregation_request_helper::parseFromBSON(
                opCtx,
                nss,
                aggCmdOnViewObj,
                verbosity,
                APIParameters::get(opCtx).getAPIStrict().value_or(false));

            auto bodyBuilder = result->getBodyBuilder();
            // An empty PrivilegeVector is acceptable because these privileges are only checked
            // on getMore and explain will not open a cursor.
            return ClusterAggregate::retryOnViewError(opCtx,
                                                      aggRequestOnView,
                                                      *ex.extraInfo<ResolvedView>(),
                                                      nss,
                                                      PrivilegeVector(),
                                                      &bodyBuilder);
        }

        long long millisElapsed = timer.millis();

        const char* mongosStageName =
            ClusterExplain::getStageNameForReadOp(shardResponses.size(), cmdObj);

        auto bodyBuilder = result->getBodyBuilder();
        return ClusterExplain::buildExplainResult(
            opCtx, shardResponses, mongosStageName, millisElapsed, cmdObj, &bodyBuilder);
    }

private:
    static long long applySkipLimit(long long num, const BSONObj& cmd) {
        BSONElement s = cmd["skip"];
        BSONElement l = cmd["limit"];

        if (s.isNumber()) {
            num = num - s.safeNumberLong();
            if (num < 0) {
                num = 0;
            }
        }

        if (l.isNumber()) {
            auto limit = l.safeNumberLong();
            if (limit < 0) {
                limit = -limit;
            }

            // 0 limit means no limit
            if (limit < num && limit != 0) {
                num = limit;
            }
        }

        return num;
    }
};

}  // namespace mongo
