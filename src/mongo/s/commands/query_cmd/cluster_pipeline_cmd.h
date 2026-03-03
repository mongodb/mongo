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

#include "mongo/base/status.h"
#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/query_cmd/extension_metrics.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/router_role/collection_routing_info_targeter.h"
#include "mongo/db/shard_role/shard_catalog/raw_data_operation.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/s/query/planner/cluster_aggregate.h"
#include "mongo/util/modules.h"

namespace mongo {

template <typename Impl>
class ClusterPipelineCommandBase final : public Command {
public:
    ClusterPipelineCommandBase() : Command(Impl::kName) {}

    const std::set<std::string>& apiVersions() const override {
        return Impl::getApiVersions();
    }

    /**
     * It's not known until after parsing whether or not an aggregation command is an explain
     * request, because it might include the `explain: true` field (ie. aggregation explains do not
     * need to arrive via the `explain` command). Therefore even parsing of regular aggregation
     * commands needs to be able to handle the explain case.
     *
     * As a result, aggregation command parsing is done in parseForExplain():
     *
     * - To parse a regular aggregation command, call parseForExplain() with `explainVerbosity` of
     *   boost::none.
     *
     * - To parse an aggregation command as the sub-command in an `explain` command, call
     *   parseForExplain() with `explainVerbosity` set to the desired verbosity.
     */
    std::unique_ptr<CommandInvocation> parse(OperationContext* opCtx,
                                             const OpMsgRequest& opMsgRequest) override {
        return parseForExplain(opCtx, opMsgRequest, boost::none);
    }

    std::unique_ptr<CommandInvocation> parseForExplain(
        OperationContext* opCtx,
        const OpMsgRequest& opMsgRequest,
        boost::optional<ExplainOptions::Verbosity> explainVerbosity) override {
        const auto aggregationRequest =
            Impl::parseAggregationRequest(opMsgRequest, explainVerbosity);

        auto privileges = uassertStatusOK(
            auth::getPrivilegesForAggregate(opCtx,
                                            AuthorizationSession::get(opCtx->getClient()),
                                            aggregationRequest.getNamespace(),
                                            aggregationRequest,
                                            true));

        // Forbid users from passing 'querySettings' explicitly.
        uassert(7708000,
                "BSON field 'querySettings' is an unknown field",
                !aggregationRequest.getQuerySettings().has_value());

        return std::make_unique<Invocation>(
            this, opMsgRequest, std::move(aggregationRequest), std::move(privileges));
    }

    class Invocation final : public CommandInvocation {
    public:
        Invocation(Command* cmd,
                   const OpMsgRequest& request,
                   AggregateCommandRequest aggregationRequest,
                   PrivilegeVector privileges)
            : CommandInvocation(cmd),
              _request(request),
              _aggregationRequest(std::move(aggregationRequest)),
              _extensionMetrics(static_cast<const ClusterPipelineCommandBase*>(cmd)
                                    ->getExtensionMetricsAllocation()),
              // Create IFRContext early to ensure consistent flag values throughout the operation,
              // including retries on view errors. Unlike mongod, mongos receives requests directly
              // from clients (which cannot include ifrFlags), so we always create an empty context
              // here.
              _ifrContext(std::make_shared<IncrementalFeatureRolloutContext>()),
              _liteParsedPipeline(
                  _aggregationRequest,
                  false /* isRunningAgainstView_ForHybridSearch */,
                  {.ifrContext = _ifrContext, .extensionMetrics = &_extensionMetrics}),
              _privileges(std::move(privileges)) {}

        const GenericArguments& getGenericArguments() const override {
            return _aggregationRequest.getGenericArguments();
        }

        bool isReadOperation() const override {
            // Only checks for the last stage since currently write stages are only allowed to be at
            // the end of the pipeline.
            return !_liteParsedPipeline.endsWithWriteStage();
        }

    private:
        bool supportsWriteConcern() const override {
            return true;
        }

        ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level,
                                                     bool isImplicitDefault) const override {
            bool isExplain = _aggregationRequest.getExplain().get_value_or(false);
            return _liteParsedPipeline.supportsReadConcern(level, isImplicitDefault, isExplain);
        }

        bool supportsRawData() const override {
            return true;
        }

        void _runAggCommand(OperationContext* opCtx,
                            BSONObjBuilder* result,
                            boost::optional<ExplainOptions::Verbosity> verbosity) {
            const auto& nss = _aggregationRequest.getNamespace();
            uassertStatusOK(ClusterAggregate::runAggregate(opCtx,
                                                           ClusterAggregate::Namespaces{nss, nss},
                                                           _aggregationRequest,
                                                           _liteParsedPipeline,
                                                           _privileges,
                                                           verbosity,
                                                           result,
                                                           "ClusterAggregate::runAggregate"_sd,
                                                           _ifrContext));
            _extensionMetrics.markSuccess();
        }

        void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* reply) override {
            CommandHelpers::handleMarkKillOnClientDisconnect(
                opCtx, !Pipeline::aggHasWriteStage(_request.body));

            Impl::checkCanRunHere(opCtx);

            auto bob = reply->getBodyBuilder();
            boost::optional<ExplainOptions::Verbosity> verbosity = boost::none;
            if (_aggregationRequest.getExplain().get_value_or(false)) {
                verbosity = ExplainOptions::Verbosity::kQueryPlanner;
            }
            _runAggCommand(opCtx, &bob, verbosity);
        }

        void explain(OperationContext* opCtx,
                     ExplainOptions::Verbosity verbosity,
                     rpc::ReplyBuilderInterface* result) override {
            Impl::checkCanExplainHere(opCtx);
            auto bodyBuilder = result->getBodyBuilder();
            _runAggCommand(opCtx, &bodyBuilder, verbosity);
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            Impl::doCheckAuthorization(opCtx, _request, _privileges);
        }

        NamespaceString ns() const override {
            return _aggregationRequest.getNamespace();
        }

        const DatabaseName& db() const override {
            return _aggregationRequest.getDbName();
        }

        const OpMsgRequest& _request;
        AggregateCommandRequest _aggregationRequest;
        ExtensionMetrics _extensionMetrics;
        // Store the IFR context as a shared pointer. This object is mutable while running the
        // command. It must be shared because commands with subpipelines pass the same IFR context
        // in to child subpipeline expression contexts, rather than copying it. We want to ensure
        // that a consistent and single IFR context is used across the entirety of query processing,
        // including in child subpipelines and retries on view errors.
        std::shared_ptr<IncrementalFeatureRolloutContext> _ifrContext;
        const LiteParsedPipeline _liteParsedPipeline;
        const PrivilegeVector _privileges;
    };

    std::string help() const override {
        return "Runs the sharded aggregation command. See "
               "http://dochub.mongodb.org/core/aggregation for more details.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kAlways;
    }

    ReadWriteType getReadWriteType() const override {
        return ReadWriteType::kRead;
    }

    /**
     * A pipeline/aggregation command does not increment the command counter, but rather increments
     * the query counter.
     */
    bool shouldAffectCommandCounter() const final {
        return false;
    }

    bool shouldAffectQueryCounter() const final {
        return true;
    }

    bool adminOnly() const override {
        return false;
    }

    const AuthorizationContract* getAuthorizationContract() const final {
        return &::mongo::AggregateCommandRequest::kAuthorizationContract;
    }

    bool allowedInTransactions() const final {
        return true;
    }

    bool enableDiagnosticPrintingOnFailure() const final {
        return true;
    }

    const ExtensionMetricsAllocation& getExtensionMetricsAllocation() const {
        tassert(
            11695900,
            "Expected cluster role to have been initialized before requesting extension metrics",
            _extensionMetricsAllocation.has_value());
        return _extensionMetricsAllocation.get();
    }

protected:
    void doInitializeClusterRole(ClusterRole role) override {
        _extensionMetricsAllocation.emplace(getName(), role);
    }

private:
    boost::optional<ExtensionMetricsAllocation> _extensionMetricsAllocation;
};

}  // namespace mongo
