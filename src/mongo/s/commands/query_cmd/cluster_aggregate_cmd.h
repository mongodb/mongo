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

#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/query_cmd/extension_metrics.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/s/query/planner/cluster_aggregate.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Implements the cluster aggregate command on both mongos (router) and shard servers.
 *
 * The 'Impl' template parameter is a small struct that provides the properties that differ between
 * the 'ClusterAggregateCmdD' and 'ClusterAggregateCmdS' variants, such as:
 *  - The command name.
 *  - The stable API version of the command via 'getApiVersions()'.
 *  - Whether the command or its explain can run via 'checkCanRunHere()' and
 * 'checkCanExplainHere()'.
 *
 * The class uses CRTP with 'TypedCommand' where:
 *  - The type ClusterAggregateCommandBase<Impl> is passed as the concrete type for the template
 * parameter.
 *  - 'Impl' provides the command-specific behavior.
 *
 * This template class provides the shared functionality between the command variants such as the
 * 'run()' and 'explain()' functions.
 *
 * See 'cluster_aggregate_cmd_s.cpp' and 'cluster_aggregate_cmd_d.cpp' for more details.
 */
template <typename Impl>
class ClusterAggregateCommandBase final : public TypedCommand<ClusterAggregateCommandBase<Impl>> {
public:
    using TC = TypedCommand<ClusterAggregateCommandBase<Impl>>;
    using Request = typename Impl::Request;
    ClusterAggregateCommandBase() : TC(Impl::kCommandName) {}

    const std::set<std::string>& apiVersions() const override {
        return Impl::getApiVersions();
    }

    class Invocation final : public TC::MinimalInvocationBase {
        using TC::MinimalInvocationBase::MinimalInvocationBase;
        using TC::MinimalInvocationBase::request;
        using TC::MinimalInvocationBase::unparsedRequest;

    public:
        Invocation(OperationContext* opCtx, Command* cmd, const OpMsgRequest& opMsgRequest)
            : TC::MinimalInvocationBase(opCtx, cmd, opMsgRequest),
              _extensionMetrics(static_cast<const ClusterAggregateCommandBase*>(cmd)
                                    ->getExtensionMetricsAllocation()),
              // Create IFRContext early to ensure consistent flag values throughout the operation,
              // including retries on view errors. Unlike mongod, mongos receives requests directly
              // from clients (which cannot include ifrFlags), so we always create an empty context
              // here.
              _ifrContext(std::make_shared<IncrementalFeatureRolloutContext>()),
              _liteParsedPipeline(
                  request(),
                  false /* isRunningAgainstView_ForHybridSearch */,
                  {.ifrContext = _ifrContext, .extensionMetrics = &_extensionMetrics}),
              _privileges(uassertStatusOK(
                  auth::getPrivilegesForAggregate(opCtx,
                                                  AuthorizationSession::get(opCtx->getClient()),
                                                  request().getNamespace(),
                                                  request(),
                                                  true))) {}

        const GenericArguments& getGenericArguments() const override {
            return request().getGenericArguments();
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
            bool isExplain = request().getExplain().get_value_or(false);
            return _liteParsedPipeline.supportsReadConcern(level, isImplicitDefault, isExplain);
        }

        bool supportsRawData() const override {
            return true;
        }

        void _runAggCommand(OperationContext* opCtx,
                            BSONObjBuilder* result,
                            boost::optional<ExplainOptions::Verbosity> verbosity) {
            if (auto pipelineForLog = _liteParsedPipeline.pipelineToBsonForLog()) {
                aggregation_request_helper::updateOpDescriptionForLog(
                    opCtx, unparsedRequest().body, *pipelineForLog);
            }

            const auto& nss = ns();
            uassertStatusOK(ClusterAggregate::runAggregate(opCtx,
                                                           ClusterAggregate::Namespaces{nss, nss},
                                                           request(),
                                                           _liteParsedPipeline,
                                                           _privileges,
                                                           verbosity,
                                                           result,
                                                           "ClusterAggregate::runAggregate"_sd,
                                                           _ifrContext));
            _extensionMetrics.markSuccess();
        }

        void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* reply) override {
            const auto& body = unparsedRequest().body;
            CommandHelpers::handleMarkKillOnClientDisconnect(opCtx,
                                                             !Pipeline::aggHasWriteStage(body));
            uassertNoQuerySettings();

            // Run aggregate-specific semantic validation beyond what the IDL-parsing provides. We
            // pass boost::none as explainVerbosity because 'validate()' interprets a non-none
            // explainVerbosity as a top-level explain.
            // TODO SERVER-119402: Change explainVerbosity parameter to bool.
            aggregation_request_helper::validate(request(), body, ns(), boost::none);

            Impl::checkCanRunHere(opCtx);

            auto bob = reply->getBodyBuilder();
            boost::optional<ExplainOptions::Verbosity> verbosity = boost::none;
            if (request().getExplain().get_value_or(false)) {
                verbosity = ExplainOptions::Verbosity::kQueryPlanner;
            }
            _runAggCommand(opCtx, &bob, verbosity);
        }

        void explain(OperationContext* opCtx,
                     ExplainOptions::Verbosity verbosity,
                     rpc::ReplyBuilderInterface* result) override {
            Impl::checkCanExplainHere(opCtx);
            uassertNoQuerySettings();

            // Mark this request as 'explain' so that downstream components such as query stats key
            // construction can see it.
            request().setExplain(true);

            // Run aggregate-specific semantic validation beyond what the IDL-parsing provides. We
            // pass boost::none as explainVerbosity because 'validate()' interprets a non-none
            // explainVerbosity as a top-level explain.
            // TODO SERVER-119402: Change explainVerbosity parameter to bool.
            aggregation_request_helper::validate(
                request(), unparsedRequest().body, ns(), verbosity);

            auto bodyBuilder = result->getBodyBuilder();
            _runAggCommand(opCtx, &bodyBuilder, verbosity);
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            Impl::doCheckAuthorization(opCtx, unparsedRequest(), _privileges);
        }

        // TODO SERVER-119513: Remove once aggregation_request_helper::validate() handles this
        // check.
        void uassertNoQuerySettings() const {
            // Forbid users from passing 'querySettings' explicitly unless the feature flag is on.
            uassert(7708000,
                    "BSON field 'querySettings' is an unknown field",
                    !request().getQuerySettings().has_value() ||
                        feature_flags::gFeatureFlagAllowUserFacingQuerySettings.isEnabled());
        }

        NamespaceString ns() const override {
            return request().getNamespace();
        }

        const DatabaseName& db() const override {
            return request().getDbName();
        }

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

    typename TC::AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return TC::AllowedOnSecondary::kAlways;
    }

    typename TC::ReadWriteType getReadWriteType() const override {
        return TC::ReadWriteType::kRead;
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
        _extensionMetricsAllocation.emplace(this->getName(), role);
    }

private:
    boost::optional<ExtensionMetricsAllocation> _extensionMetricsAllocation;
};

}  // namespace mongo
