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

#include <boost/optional.hpp>

#include "mongo/client/read_preference.h"
#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/commands.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/query_request_conversion.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/query_settings/query_settings_utils.h"
#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/db/query/query_stats/find_key.h"
#include "mongo/db/query/query_stats/query_stats.h"
#include "mongo/db/query/query_utils.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/commands/cluster_explain.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_aggregate.h"
#include "mongo/s/query/cluster_find.h"

namespace mongo {

/**
 * Implements the find command for a router.
 */
template <typename Impl>
class ClusterFindCmdBase final : public Command {
public:
    static constexpr StringData kTermField = "term"_sd;

    ClusterFindCmdBase() : Command(Impl::kName) {}

    const std::set<std::string>& apiVersions() const {
        return Impl::getApiVersions();
    }

    std::unique_ptr<CommandInvocation> parse(OperationContext* opCtx,
                                             const OpMsgRequest& opMsgRequest) override {
        // TODO: Parse into a QueryRequest here.
        return std::make_unique<Invocation>(this, opMsgRequest);
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
    bool shouldAffectCommandCounter() const override final {
        return false;
    }

    bool shouldAffectQueryCounter() const override final {
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
        Invocation(const ClusterFindCmdBase* definition, const OpMsgRequest& request)
            : CommandInvocation(definition), _request(request), _dbName(request.getDbName()) {}

    private:
        bool supportsWriteConcern() const override {
            return false;
        }

        ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level,
                                                     bool isImplicitDefault) const final {
            return ReadConcernSupportResult::allSupportedAndDefaultPermitted();
        }

        NamespaceString ns() const override {
            // TODO get the ns from the parsed QueryRequest.
            return NamespaceString(
                CommandHelpers::parseNsCollectionRequired(_dbName, _request.body));
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

            auto findCommand = _parseCmdObjectToFindCommandRequest(opCtx, ns(), _request.body);
            auto expCtx = makeExpressionContext(opCtx, *findCommand, verbosity);
            auto parsedFind = uassertStatusOK(parsed_find_command::parse(
                expCtx,
                {.findCommand = std::move(findCommand),
                 .allowedFeatures = MatchExpressionParser::kAllowAllSpecialFeatures}));

            // Update 'findCommand' by setting the looked up query settings, such that they can be
            // applied on the shards.
            auto querySettings =
                query_settings::lookupQuerySettingsForFind(expCtx, *parsedFind, ns());
            findCommand = std::move(parsedFind->findCommandRequest);
            if (!query_settings::utils::isDefault(querySettings)) {
                findCommand->setQuerySettings(std::move(querySettings));
            }

            try {
                const auto explainCmd =
                    ClusterExplain::wrapAsExplain(findCommand->toBSON(BSONObj()), verbosity);

                long long millisElapsed;
                std::vector<AsyncRequestsSender::Response> shardResponses;

                // We will time how long it takes to run the commands on the shards.
                Timer timer;
                const auto cri =
                    uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(
                        opCtx, findCommand->getNamespaceOrUUID().nss()));
                shardResponses = scatterGatherVersionedTargetByRoutingTable(
                    opCtx,
                    findCommand->getNamespaceOrUUID().nss().dbName(),
                    findCommand->getNamespaceOrUUID().nss(),
                    cri,
                    explainCmd,
                    ReadPreferenceSetting::get(opCtx),
                    Shard::RetryPolicy::kIdempotent,
                    findCommand->getFilter(),
                    findCommand->getCollation(),
                    findCommand->getLet(),
                    findCommand->getLegacyRuntimeConstants());
                millisElapsed = timer.millis();

                const char* mongosStageName =
                    ClusterExplain::getStageNameForReadOp(shardResponses.size(), _request.body);

                auto bodyBuilder = result->getBodyBuilder();
                uassertStatusOK(ClusterExplain::buildExplainResult(opCtx,
                                                                   shardResponses,
                                                                   mongosStageName,
                                                                   millisElapsed,
                                                                   _request.body,
                                                                   &bodyBuilder));

            } catch (const ExceptionFor<ErrorCodes::CommandOnShardedViewNotSupportedOnMongod>& ex) {
                retryOnViewError(opCtx,
                                 result,
                                 *findCommand,
                                 *ex.extraInfo<ResolvedView>(),
                                 // An empty PrivilegeVector is acceptable because these privileges
                                 // are only checked on getMore and explain will not open a cursor.
                                 {},
                                 verbosity);

            } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
                auto bodyBuilder = result->getBodyBuilder();
                auto findRequest = _parseCmdObjectToFindCommandRequest(opCtx, ns(), _request.body);
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

        void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* result) {
            Impl::checkCanRunHere(opCtx);

            CommandHelpers::handleMarkKillOnClientDisconnect(opCtx);

            auto findCommand = _parseCmdObjectToFindCommandRequest(opCtx, ns(), _request.body);
            auto expCtx = makeExpressionContext(opCtx, *findCommand);
            auto parsedFind = uassertStatusOK(parsed_find_command::parse(
                expCtx,
                {.findCommand = std::move(findCommand),
                 .allowedFeatures = MatchExpressionParser::kAllowAllSpecialFeatures}));

            registerRequestForQueryStats(expCtx, *parsedFind);

            // Perform the query settings lookup and attach it to 'expCtx'.
            expCtx->setQuerySettings(
                query_settings::lookupQuerySettingsForFind(expCtx, *parsedFind, ns()));

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
                    *ex.extraInfo<ResolvedView>(),
                    {Privilege(ResourcePattern::forExactNamespace(ns()), ActionType::find)});
            }
        }

    private:
        /**
         * Parses the command object to a FindCommandRequest, validates that no runtime
         * constants were supplied with the command, and sets the constant runtime values that
         * will be forwarded to each shard.
         */
        std::unique_ptr<FindCommandRequest> _parseCmdObjectToFindCommandRequest(
            OperationContext* opCtx, NamespaceString nss, BSONObj cmdObj) {
            auto findCommand = query_request_helper::makeFromFindCommand(
                std::move(cmdObj),
                auth::ValidatedTenancyScope::get(opCtx),
                nss.tenantId(),
                SerializationContext::stateDefault(),
                APIParameters::get(opCtx).getAPIStrict().value_or(false));
            if (!findCommand->getReadConcern()) {
                if (opCtx->isStartingMultiDocumentTransaction() ||
                    !opCtx->inMultiDocumentTransaction()) {
                    // If there is no explicit readConcern in the cmdObj, and this is either the
                    // first operation in a transaction, or not running in a transaction, then
                    // use the readConcern from the opCtx (which may be a cluster-wide default).
                    const auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
                    findCommand->setReadConcern(readConcernArgs.toBSONInner());
                }
            }
            uassert(51202,
                    "Cannot specify runtime constants option to a mongos",
                    !findCommand->getLegacyRuntimeConstants());

            // Forbid users from passing 'querySettings' explicitly.
            uassert(7746900,
                    "BSON field 'querySettings' is an unknown field",
                    !findCommand->getQuerySettings().has_value());

            if (shouldDoFLERewrite(findCommand)) {
                invariant(findCommand->getNamespaceOrUUID().isNamespaceString());

                if (!findCommand->getEncryptionInformation()->getCrudProcessed().value_or(false)) {
                    processFLEFindS(
                        opCtx, findCommand->getNamespaceOrUUID().nss(), findCommand.get());
                    _didDoFLERewrite = true;
                }
                {
                    stdx::lock_guard<Client> lk(*opCtx->getClient());
                    CurOp::get(opCtx)->setShouldOmitDiagnosticInformation_inlock(lk, true);
                }
            }

            return findCommand;
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
            const ResolvedView& resolvedView,
            const PrivilegeVector& privileges,
            boost::optional<mongo::ExplainOptions::Verbosity> verbosity = boost::none) {
            auto bodyBuilder = result->getBodyBuilder();
            bodyBuilder.resetToEmpty();

            auto aggRequestOnView =
                query_request_conversion::asAggregateCommandRequest(findCommand);
            aggRequestOnView.setExplain(verbosity);

            uassertStatusOK(ClusterAggregate::retryOnViewError(
                opCtx, aggRequestOnView, resolvedView, ns(), privileges, &bodyBuilder));
        }

        const OpMsgRequest& _request;
        const DatabaseName _dbName;
        bool _didDoFLERewrite{false};
    };
};

}  // namespace mongo
