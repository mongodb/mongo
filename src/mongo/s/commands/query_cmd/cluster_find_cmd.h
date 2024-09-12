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

#include "mongo/db/query/find_command.h"
#include "mongo/db/query/parsed_find_command.h"
#include <boost/optional.hpp>

#include "mongo/client/read_preference.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/commands.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/pipeline/query_request_conversion.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/db/query/query_settings/query_settings_utils.h"
#include "mongo/db/query/query_stats/find_key.h"
#include "mongo/db/query/query_stats/query_stats.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/idl/generic_argument_gen.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/commands/query_cmd/cluster_explain.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/planner/cluster_aggregate.h"
#include "mongo/s/query/planner/cluster_find.h"

namespace mongo {
namespace {
/**
 * Parses the command object to a FindCommandRequest and validates that no runtime
 * constants were supplied and that querySettings was not passed into the command.
 */
std::unique_ptr<FindCommandRequest> parseCmdObjectToFindCommandRequest(
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

    return findCommand;
}
}  // namespace

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

            auto expCtx = makeExpressionContext(opCtx, *_cmdRequest, verbosity);
            auto parsedFind = uassertStatusOK(parsed_find_command::parse(
                expCtx,
                {.findCommand = std::move(_cmdRequest),
                 .allowedFeatures = MatchExpressionParser::kAllowAllSpecialFeatures}));

            // Update 'findCommand' by setting the looked up query settings, such that they can be
            // applied on the shards.
            auto querySettings =
                query_settings::lookupQuerySettingsForFind(expCtx, *parsedFind, ns());
            expCtx->setQuerySettingsIfNotPresent(querySettings);

            auto cq = CanonicalQuery(CanonicalQueryParams{
                .expCtx = std::move(expCtx),
                .parsedFind = std::move(parsedFind),
            });

            _cmdRequest = std::make_unique<FindCommandRequest>(cq.getFindCommandRequest());
            expCtx = makeExpressionContext(opCtx, *_cmdRequest, verbosity);

            try {
                long long millisElapsed;
                std::vector<AsyncRequestsSender::Response> shardResponses;

                // We will time how long it takes to run the commands on the shards.
                Timer timer;
                const auto cri =
                    uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(
                        opCtx, cq.getFindCommandRequest().getNamespaceOrUUID().nss()));

                auto numShards = getTargetedShardsForCanonicalQuery(cq, cri.cm).size();
                // When forwarding the command to multiple shards, need to transform it by adjusting
                // query parameters such as limits and sorts.
                if (numShards > 1) {
                    _cmdRequest = uassertStatusOK(ClusterFind::transformQueryForShards(cq));
                }

                const auto explainCmd = ClusterExplain::wrapAsExplain(
                    _cmdRequest->toBSON(), verbosity, querySettings.toBSON());

                shardResponses = scatterGatherVersionedTargetByRoutingTable(
                    opCtx,
                    _cmdRequest->getNamespaceOrUUID().nss().dbName(),
                    _cmdRequest->getNamespaceOrUUID().nss(),
                    cri,
                    explainCmd,
                    ReadPreferenceSetting::get(opCtx),
                    Shard::RetryPolicy::kIdempotent,
                    _cmdRequest->getFilter(),
                    _cmdRequest->getCollation(),
                    _cmdRequest->getLet(),
                    _cmdRequest->getLegacyRuntimeConstants());
                millisElapsed = timer.millis();

                const char* mongosStageName =
                    ClusterExplain::getStageNameForReadOp(shardResponses.size(), _request.body);

                auto bodyBuilder = result->getBodyBuilder();
                uassertStatusOK(ClusterExplain::buildExplainResult(expCtx,
                                                                   shardResponses,
                                                                   mongosStageName,
                                                                   millisElapsed,
                                                                   _request.body,
                                                                   &bodyBuilder));

            } catch (const ExceptionFor<ErrorCodes::CommandOnShardedViewNotSupportedOnMongod>& ex) {
                retryOnViewError(opCtx,
                                 result,
                                 *_cmdRequest,
                                 querySettings,
                                 *ex.extraInfo<ResolvedView>(),
                                 // An empty PrivilegeVector is acceptable because these privileges
                                 // are only checked on getMore and explain will not open a cursor.
                                 {},
                                 verbosity);

            } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
                auto bodyBuilder = result->getBodyBuilder();
                auto findRequest = parseCmdObjectToFindCommandRequest(opCtx, _request);
                setReadConcern(opCtx);
                doFLERewriteIfNeeded(opCtx);
                auto expCtx = make_intrusive<ExpressionContext>(
                    opCtx, *findRequest, nullptr, true /* mayDbProfile */);
                auto&& parsedFindResult = uassertStatusOK(parsed_find_command::parse(
                    expCtx,
                    {.findCommand = std::move(findRequest),
                     .allowedFeatures = MatchExpressionParser::kAllowAllSpecialFeatures}));
                auto cq = std::make_unique<CanonicalQuery>(CanonicalQueryParams{
                    .expCtx = std::move(expCtx),
                    .parsedFind = std::move(parsedFindResult),
                });
                ClusterExplain::buildEOFExplainResult(opCtx, cq.get(), _request.body, &bodyBuilder);
            }
        }

        void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* result) override {
            Impl::checkCanRunHere(opCtx);
            CommandHelpers::handleMarkKillOnClientDisconnect(opCtx);

            setReadConcern(opCtx);
            doFLERewriteIfNeeded(opCtx);

            auto expCtx = makeExpressionContext(opCtx, *_cmdRequest);
            auto parsedFind = uassertStatusOK(parsed_find_command::parse(
                expCtx,
                {.findCommand = std::move(_cmdRequest),
                 .allowedFeatures = MatchExpressionParser::kAllowAllSpecialFeatures}));

            registerRequestForQueryStats(expCtx, *parsedFind);

            // Perform the query settings lookup and attach it to 'expCtx'.
            auto querySettings =
                query_settings::lookupQuerySettingsForFind(expCtx, *parsedFind, ns());
            expCtx->setQuerySettingsIfNotPresent(querySettings);

            auto cq = std::make_unique<CanonicalQuery>(CanonicalQueryParams{
                .expCtx = std::move(expCtx), .parsedFind = std::move(parsedFind)});

            try {
                // Do the work to generate the first batch of results. This blocks waiting to
                // get responses from the shard(s).
                bool partialResultsReturned = false;
                std::vector<BSONObj> batch;
                auto cursorId = ClusterFind::runQuery(
                    opCtx, *cq, ReadPreferenceSetting::get(opCtx), &batch, &partialResultsReturned);

                // Build the response document.
                CursorResponseBuilder::Options options;
                options.isInitialResponse = true;
                if (!opCtx->inMultiDocumentTransaction()) {
                    options.atClusterTime =
                        repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime();
                }
                CursorResponseBuilder firstBatch(result, options);
                for (const auto& obj : batch) {
                    firstBatch.append(obj);
                }
                firstBatch.setPartialResultsReturned(partialResultsReturned);
                firstBatch.done(cursorId, cq->nss());
            } catch (const ExceptionFor<ErrorCodes::CommandOnShardedViewNotSupportedOnMongod>& ex) {
                retryOnViewError(
                    opCtx,
                    result,
                    cq->getFindCommandRequest(),
                    querySettings,
                    *ex.extraInfo<ResolvedView>(),
                    {Privilege(ResourcePattern::forExactNamespace(ns()), ActionType::find)});
            }
        }

        const GenericArguments& getGenericArguments() const override {
            return _genericArgs;
        }

        void registerRequestForQueryStats(const boost::intrusive_ptr<ExpressionContext> expCtx,
                                          const ParsedFindCommand& parsedFind) {
            if (!_didDoFLERewrite) {
                query_stats::registerRequest(expCtx->opCtx, expCtx->ns, [&]() {
                    // This callback is either never invoked or invoked immediately within
                    // registerRequest, so use-after-move of parsedFind isn't an issue.
                    return std::make_unique<query_stats::FindKey>(expCtx, parsedFind);
                });
            }
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

            auto aggRequestOnView =
                query_request_conversion::asAggregateCommandRequest(findCommand);
            aggRequestOnView.setExplain(verbosity);
            if (!query_settings::utils::isDefault(querySettings)) {
                aggRequestOnView.setQuerySettings(querySettings);
            }

            uassertStatusOK(ClusterAggregate::retryOnViewError(
                opCtx, aggRequestOnView, resolvedView, ns(), privileges, &bodyBuilder));
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
            if (!shouldDoFLERewrite(_cmdRequest)) {
                return;
            }

            invariant(_cmdRequest->getNamespaceOrUUID().isNamespaceString());
            if (!_cmdRequest->getEncryptionInformation()->getCrudProcessed().value_or(false)) {
                processFLEFindS(opCtx, _cmdRequest->getNamespaceOrUUID().nss(), _cmdRequest.get());
                _didDoFLERewrite = true;
            }
            {
                stdx::lock_guard<Client> lk(*opCtx->getClient());
                CurOp::get(opCtx)->setShouldOmitDiagnosticInformation_inlock(lk, true);
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
