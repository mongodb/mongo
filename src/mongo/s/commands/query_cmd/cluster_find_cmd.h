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

#include "mongo/client/read_preference.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/commands.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/expression_context_diagnostic_printer.h"
#include "mongo/db/pipeline/query_request_conversion.h"
#include "mongo/db/pipeline/sharded_agg_helpers.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/parsed_find_command.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/db/query/query_settings/query_settings_service.h"
#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/db/query/query_stats/find_key.h"
#include "mongo/db/query/query_stats/query_stats.h"
#include "mongo/db/query/shard_key_diagnostic_printer.h"
#include "mongo/db/query/util/cluster_find_util.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/router_role/collection_routing_info_targeter.h"
#include "mongo/db/router_role/router_role.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/shard_role/shard_catalog/raw_data_operation.h"
#include "mongo/db/timeseries/timeseries_request_util.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/idl/generic_argument_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/commands/query_cmd/cluster_explain.h"
#include "mongo/s/query/planner/cluster_aggregate.h"
#include "mongo/s/query/planner/cluster_find.h"

#include <boost/optional.hpp>

namespace mongo {
/**
 * Parses the command object to a FindCommandRequest and validates that no runtime
 * constants were supplied and that querySettings was not passed into the command.
 */
inline std::unique_ptr<FindCommandRequest> parseCmdObjectToFindCommandRequest(
    OperationContext* opCtx, const OpMsgRequest& request) {
    const auto& vts = auth::ValidatedTenancyScope::get(opCtx);
    auto findCommand = query_request_helper::makeFromFindCommand(
        request.body,
        vts,
        vts.has_value() ? boost::make_optional(vts->tenantId()) : boost::none,
        SerializationContext::stateDefault());

    uassert(51202,
            "Cannot specify runtime constants option to a mongos",
            !findCommand->getLegacyRuntimeConstants());

    // Forbid users from passing 'querySettings' explicitly.
    uassert(7746900,
            "BSON field 'querySettings' is an unknown field",
            !findCommand->getQuerySettings().has_value());

    uassert(ErrorCodes::InvalidNamespace,
            "Cannot specify UUID to a mongos.",
            !findCommand->getNamespaceOrUUID().isUUID());

    uassert(ErrorCodes::InvalidNamespace,
            "Cannot specify find without a real namespace",
            !findCommand->getNamespaceOrUUID().nss().isCollectionlessAggregateNS());

    return findCommand;
}

/**
 * Implements the find command for a router.
 */
template <typename Impl>
class ClusterFindCmdBase final : public Command {
public:
    static constexpr StringData kTermField = "term"_sd;

    ClusterFindCmdBase() : Command(Impl::kName) {}

    const std::set<std::string>& apiVersions() const override {
        return Impl::getApiVersions();
    }

    std::unique_ptr<CommandInvocation> parse(OperationContext* opCtx,
                                             const OpMsgRequest& opMsgRequest) override {
        auto cmdRequest = parseCmdObjectToFindCommandRequest(opCtx, opMsgRequest);
        return std::make_unique<Invocation>(this, opMsgRequest, std::move(cmdRequest));
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext* context) const override {
        return AllowedOnSecondary::kOptIn;
    }

    bool maintenanceOk() const final {
        return false;
    }

    bool adminOnly() const final {
        return false;
    }

    ReadWriteType getReadWriteType() const final {
        return ReadWriteType::kRead;
    }

    /**
     * A find command does not increment the command counter, but rather increments the
     * query counter.
     */
    bool shouldAffectCommandCounter() const final {
        return false;
    }

    bool shouldAffectQueryCounter() const final {
        return true;
    }

    bool allowedInTransactions() const final {
        return true;
    }

    std::string help() const override {
        return "query for documents";
    }

    bool enableDiagnosticPrintingOnFailure() const final {
        return true;
    }

    class Invocation final : public CommandInvocation {
    public:
        Invocation(const ClusterFindCmdBase* definition,
                   const OpMsgRequest& request,
                   std::unique_ptr<FindCommandRequest> cmdRequest)
            : CommandInvocation(definition),
              _request(request),
              _ns(cmdRequest->getNamespaceOrUUID().nss()),
              _genericArgs(cmdRequest->getGenericArguments()),
              _cmdRequest(std::move(cmdRequest)) {}

    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level,
                                                     bool isImplicitDefault) const final {
            return ReadConcernSupportResult::allSupportedAndDefaultPermitted();
        }

        bool supportsRawData() const override {
            return true;
        }

        NamespaceString ns() const override {
            return _ns;
        }

        const DatabaseName& db() const override {
            return _ns.dbName();
        }

        /**
         * In order to run the find command, you must be authorized for the "find" action
         * type on the collection.
         */
        void doCheckAuthorization(OperationContext* opCtx) const final {
            auto hasTerm = _request.body.hasField(kTermField);
            Impl::doCheckAuthorization(opCtx, hasTerm, ns());
        }

        void explain(OperationContext* opCtx,
                     ExplainOptions::Verbosity verbosity,
                     rpc::ReplyBuilderInterface* result) override {
            Impl::checkCanExplainHere(opCtx);

            auto curOp = CurOp::get(opCtx);
            curOp->debug().queryStatsInfo.disableForSubqueryExecution = true;

            setReadConcern(opCtx);
            doFLERewriteIfNeeded(opCtx);

            auto findBodyFn = [&](OperationContext* opCtx, RoutingContext& originalRoutingCtx) {
                // Clear the bodyBuilder since this lambda function may be retried if the
                // router cache is stale.
                result->getBodyBuilder().resetToEmpty();

                // Transform the nss, routingCtx and cmdObj if the 'rawData' field is enabled and
                // the collection is timeseries.
                auto nss = ns();
                auto cmdRequest = std::make_unique<FindCommandRequest>(*_cmdRequest);
                bool cmdShouldBeTranslatedForRawData = false;
                const auto targeter = CollectionRoutingInfoTargeter(opCtx, ns());
                auto& routingCtx = translateNssForRawDataAccordingToRoutingInfo(
                    opCtx,
                    ns(),
                    targeter,
                    originalRoutingCtx,
                    [&](const NamespaceString& translatedNss) {
                        cmdRequest->setNss(translatedNss);
                        nss = translatedNss;
                        cmdShouldBeTranslatedForRawData = true;
                    });

                auto query = ClusterFind::generateAndValidateCanonicalQuery(
                    opCtx,
                    ns(),
                    std::move(cmdRequest),
                    verbosity,
                    MatchExpressionParser::kAllowAllSpecialFeatures,
                    false /* mustRegisterRequestToQueryStats */);

                // Create an RAII object that prints useful information about the
                // ExpressionContext in the case of a tassert or crash.
                ScopedDebugInfo expCtxDiagnostics(
                    "ExpCtxDiagnostics",
                    diagnostic_printers::ExpressionContextPrinter{query->getExpCtx()});

                // We will time how long it takes to run the commands on the shards.
                Timer timer;

                // Handle requests against a viewless timeseries collection.
                if (auto cursorId =
                        cluster_find_util::convertFindAndRunAggregateIfViewlessTimeseries(
                            opCtx,
                            routingCtx,
                            ns(),
                            result,
                            query->getFindCommandRequest(),
                            query->getExpCtx()->getQuerySettings(),
                            verbosity)) {
                    return;
                }

                const auto& cri = routingCtx.getCollectionRoutingInfo(nss);

                // Create an RAII object that prints the collection's shard key in the case
                // of a tassert or crash.
                ScopedDebugInfo shardKeyDiagnostics(
                    "ShardKeyDiagnostics",
                    diagnostic_printers::ShardKeyDiagnosticPrinter{
                        cri.isSharded() ? cri.getChunkManager().getShardKeyPattern().toBSON()
                                        : BSONObj()});

                auto numShards = getTargetedShardsForCanonicalQuery(*query, cri).size();
                // When forwarding the command to multiple shards, need to transform it by
                // adjusting query parameters such as limits and sorts.
                auto userLimit = query->getFindCommandRequest().getLimit();
                auto userSkip = query->getFindCommandRequest().getSkip();
                if (numShards > 1) {
                    cmdRequest = uassertStatusOK(ClusterFind::transformQueryForShards(*query));
                } else {
                    // Forwards the FindCommandRequest as is to a single shard so that limit and
                    // skip can be applied on mongod.
                    cmdRequest =
                        std::make_unique<FindCommandRequest>(query->getFindCommandRequest());
                }

                const auto explainCmd = ClusterExplain::wrapAsExplain(
                    cmdShouldBeTranslatedForRawData
                        ? rewriteCommandForRawDataOperation<FindCommandRequest>(
                              cmdRequest->toBSON(), nss.coll())
                        : cmdRequest->toBSON(),
                    verbosity,
                    query->getExpCtx()->getQuerySettings().toBSON());

                try {
                    auto shardResponses = scatterGatherVersionedTargetByRoutingTable(
                        opCtx,
                        routingCtx,
                        nss,
                        explainCmd,
                        ReadPreferenceSetting::get(opCtx),
                        Shard::RetryPolicy::kIdempotent,
                        cmdRequest->getFilter(),
                        cmdRequest->getCollation(),
                        cmdRequest->getLet(),
                        cmdRequest->getLegacyRuntimeConstants());

                    long long millisElapsed = timer.millis();

                    const char* mongosStageName =
                        ClusterExplain::getStageNameForReadOp(shardResponses.size(), _request.body);

                    auto bodyBuilder = result->getBodyBuilder();
                    uassertStatusOK(ClusterExplain::buildExplainResult(query->getExpCtx(),
                                                                       shardResponses,
                                                                       mongosStageName,
                                                                       millisElapsed,
                                                                       _request.body,
                                                                       &bodyBuilder,
                                                                       userLimit,
                                                                       userSkip));
                } catch (
                    const ExceptionFor<ErrorCodes::CommandOnShardedViewNotSupportedOnMongod>& ex) {
                    retryOnViewError(
                        opCtx,
                        result,
                        *cmdRequest,
                        query->getExpCtx()->getQuerySettings(),
                        *ex.extraInfo<ResolvedView>(),
                        // An empty PrivilegeVector is acceptable because these privileges
                        // are only checked on getMore and explain will not open a cursor.
                        {},
                        verbosity);
                }
            };

            try {
                sharding::router::CollectionRouter router{opCtx->getServiceContext(), ns()};
                router.routeWithRoutingContext(opCtx, "explain find"_sd, findBodyFn);

            } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
                auto bodyBuilder = result->getBodyBuilder();

                auto findRequest = parseCmdObjectToFindCommandRequest(opCtx, _request);
                auto query = ClusterFind::generateAndValidateCanonicalQuery(
                    opCtx,
                    ns(),
                    std::move(findRequest),
                    verbosity,
                    MatchExpressionParser::kAllowAllSpecialFeatures,
                    false /* mustRegisterRequestToQueryStats */);

                ClusterExplain::buildEOFExplainResult(
                    opCtx, query.get(), _request.body, &bodyBuilder);
            }
        }

        void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* result) override {
            Impl::checkCanRunHere(opCtx);
            CommandHelpers::handleMarkKillOnClientDisconnect(opCtx);

            setReadConcern(opCtx);
            doFLERewriteIfNeeded(opCtx);

            ClusterFind::runQuery(opCtx,
                                  std::move(_cmdRequest),
                                  ns(),
                                  ReadPreferenceSetting::get(opCtx),
                                  MatchExpressionParser::kAllowAllSpecialFeatures,
                                  result,
                                  _didDoFLERewrite);
        }

        const GenericArguments& getGenericArguments() const override {
            return _genericArgs;
        }

        void retryOnViewError(
            OperationContext* opCtx,
            rpc::ReplyBuilderInterface* result,
            const FindCommandRequest& findCommand,
            const query_settings::QuerySettings& querySettings,
            const ResolvedView& resolvedView,
            const PrivilegeVector& privileges,
            boost::optional<mongo::ExplainOptions::Verbosity> verbosity = boost::none) {
            auto bodyBuilder = result->getBodyBuilder();
            bodyBuilder.resetToEmpty();
            bool hasExplain = verbosity.has_value();
            auto aggRequestOnView =
                query_request_conversion::asAggregateCommandRequest(findCommand, hasExplain);

            if (!query_settings::isDefault(querySettings)) {
                aggRequestOnView.setQuerySettings(querySettings);
            }

            uassertStatusOK(ClusterAggregate::retryOnViewError(
                opCtx, aggRequestOnView, resolvedView, ns(), privileges, verbosity, &bodyBuilder));
        }

        void setReadConcern(OperationContext* opCtx) {
            if (_cmdRequest->getReadConcern() ||
                (opCtx->inMultiDocumentTransaction() &&
                 !opCtx->isStartingMultiDocumentTransaction())) {
                return;
            }

            // Use the readConcern from the opCtx (which may be a cluster-wide default).
            const auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
            _cmdRequest->setReadConcern(readConcernArgs);
        }

        void doFLERewriteIfNeeded(OperationContext* opCtx) {
            if (prepareForFLERewrite(opCtx, _cmdRequest->getEncryptionInformation())) {
                tassert(9483401,
                        "Expecting namespace string for find command",
                        _cmdRequest->getNamespaceOrUUID().isNamespaceString());
                processFLEFindS(opCtx, _cmdRequest->getNamespaceOrUUID().nss(), _cmdRequest.get());
                _didDoFLERewrite = true;
            }
        }

        const OpMsgRequest& _request;
        const NamespaceString _ns;
        bool _didDoFLERewrite{false};
        const GenericArguments _genericArgs;
        std::unique_ptr<FindCommandRequest> _cmdRequest;
    };
};

}  // namespace mongo
