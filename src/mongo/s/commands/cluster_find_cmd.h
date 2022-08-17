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
#include "mongo/db/commands.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/commands/cluster_explain.h"
#include "mongo/s/grid.h"
#include "mongo/s/is_mongos.h"
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

    bool shouldAffectCommandCounter() const final {
        return false;
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
            : CommandInvocation(definition),
              _request(request),
              _dbName(request.getValidatedTenantId(), request.getDatabase()) {}

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
            // Parse the command BSON to a FindCommandRequest.
            auto findCommand = _parseCmdObjectToFindCommandRequest(opCtx, ns(), _request.body);

            try {
                const auto explainCmd =
                    ClusterExplain::wrapAsExplain(findCommand->toBSON(BSONObj()), verbosity);

                Impl::checkCanRunHere(opCtx);

                long long millisElapsed;
                std::vector<AsyncRequestsSender::Response> shardResponses;

                // We will time how long it takes to run the commands on the shards.
                Timer timer;
                const auto routingInfo =
                    uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(
                        opCtx, *findCommand->getNamespaceOrUUID().nss()));
                shardResponses = scatterGatherVersionedTargetByRoutingTable(
                    opCtx,
                    findCommand->getNamespaceOrUUID().nss()->db(),
                    *findCommand->getNamespaceOrUUID().nss(),
                    routingInfo,
                    explainCmd,
                    ReadPreferenceSetting::get(opCtx),
                    Shard::RetryPolicy::kIdempotent,
                    findCommand->getFilter(),
                    findCommand->getCollation());
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
                auto bodyBuilder = result->getBodyBuilder();
                bodyBuilder.resetToEmpty();

                auto aggCmdOnView =
                    uassertStatusOK(query_request_helper::asAggregationCommand(*findCommand));
                auto viewAggregationCommand =
                    OpMsgRequest::fromDBAndBody(_dbName.db(), aggCmdOnView).body;

                auto aggRequestOnView = aggregation_request_helper::parseFromBSON(
                    opCtx,
                    ns(),
                    viewAggregationCommand,
                    verbosity,
                    APIParameters::get(opCtx).getAPIStrict().value_or(false));

                // An empty PrivilegeVector is acceptable because these privileges are only checked
                // on getMore and explain will not open a cursor.
                uassertStatusOK(ClusterAggregate::retryOnViewError(opCtx,
                                                                   aggRequestOnView,
                                                                   *ex.extraInfo<ResolvedView>(),
                                                                   ns(),
                                                                   PrivilegeVector(),
                                                                   &bodyBuilder));
            }
        }

        void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* result) {
            CommandHelpers::handleMarkKillOnClientDisconnect(opCtx);
            // We count find command as a query op.
            globalOpCounters.gotQuery();

            Impl::checkCanRunHere(opCtx);

            ON_BLOCK_EXIT([opCtx] {
                Grid::get(opCtx)->catalogCache()->checkAndRecordOperationBlockedByRefresh(
                    opCtx, mongo::LogicalOp::opQuery);
            });

            auto findCommand = _parseCmdObjectToFindCommandRequest(opCtx, ns(), _request.body);

            const boost::intrusive_ptr<ExpressionContext> expCtx;
            auto cq = uassertStatusOK(
                CanonicalQuery::canonicalize(opCtx,
                                             std::move(findCommand),
                                             false, /* isExplain */
                                             expCtx,
                                             ExtensionsCallbackNoop(),
                                             MatchExpressionParser::kAllowAllSpecialFeatures));

            try {
                // Do the work to generate the first batch of results. This blocks waiting to get
                // responses from the shard(s).
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
                firstBatch.done(cursorId, cq->ns());
            } catch (const ExceptionFor<ErrorCodes::CommandOnShardedViewNotSupportedOnMongod>& ex) {
                result->reset();

                auto aggCmdOnView = uassertStatusOK(
                    query_request_helper::asAggregationCommand(cq->getFindCommandRequest()));
                auto viewAggregationCommand =
                    OpMsgRequest::fromDBAndBody(_dbName.db(), aggCmdOnView).body;

                auto aggRequestOnView = aggregation_request_helper::parseFromBSON(
                    opCtx,
                    ns(),
                    viewAggregationCommand,
                    boost::none,
                    APIParameters::get(opCtx).getAPIStrict().value_or(false));

                auto bodyBuilder = result->getBodyBuilder();
                uassertStatusOK(ClusterAggregate::retryOnViewError(
                    opCtx,
                    aggRequestOnView,
                    *ex.extraInfo<ResolvedView>(),
                    ns(),
                    {Privilege(ResourcePattern::forExactNamespace(ns()), ActionType::find)},
                    &bodyBuilder));
            }
        }

    private:
        /**
         * Parses the command object to a FindCommandRequest, validates that no runtime constants
         * were supplied with the command, and sets the constant runtime values that will be
         * forwarded to each shard.
         */
        static std::unique_ptr<FindCommandRequest> _parseCmdObjectToFindCommandRequest(
            OperationContext* opCtx, NamespaceString nss, BSONObj cmdObj) {
            auto findCommand = query_request_helper::makeFromFindCommand(
                std::move(cmdObj),
                std::move(nss),
                APIParameters::get(opCtx).getAPIStrict().value_or(false));
            if (!findCommand->getReadConcern()) {
                if (opCtx->isStartingMultiDocumentTransaction() ||
                    !opCtx->inMultiDocumentTransaction()) {
                    // If there is no explicit readConcern in the cmdObj, and this is either the
                    // first operation in a transaction, or not running in a transaction, then use
                    // the readConcern from the opCtx (which may be a cluster-wide default).
                    const auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
                    findCommand->setReadConcern(readConcernArgs.toBSONInner());
                }
            }
            uassert(51202,
                    "Cannot specify runtime constants option to a mongos",
                    !findCommand->getLegacyRuntimeConstants());

            if (shouldDoFLERewrite(findCommand)) {
                invariant(findCommand->getNamespaceOrUUID().nss());
                processFLEFindS(
                    opCtx, findCommand->getNamespaceOrUUID().nss().get(), findCommand.get());
            }

            return findCommand;
        }

        const OpMsgRequest& _request;
        const DatabaseName _dbName;
    };
};

}  // namespace mongo
