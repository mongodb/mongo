// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/change_stream_metrics_util.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/query_cmd/extension_metrics.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/router_role/cluster_commands_helpers.h"
#include "mongo/db/stats/counters.h"
#include "mongo/s/query/planner/cluster_aggregate.h"
#include "mongo/util/modules.h"

namespace mongo {
using namespace std::literals::string_view_literals;

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

    bool supportsQuerySettings() const override {
        return true;
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
              _ifrContext(IncrementalFeatureRolloutContext::get(opCtx)),
              _liteParsedPipeline(request(),
                                  false /* isRunningAgainstView_ForHybridSearch */,
                                  {.ifrContext = _ifrContext,
                                   .opCtx = opCtx,
                                   .extensionMetrics = &_extensionMetrics}),
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

            setReadWriteConcern(opCtx, request(), true /* setRC */, !verbosity /* setWC */);

            const auto& nss = ns();
            uassertStatusOK(ClusterAggregate::runAggregate(opCtx,
                                                           ClusterAggregate::Namespaces{nss, nss},
                                                           request(),
                                                           _liteParsedPipeline,
                                                           _privileges,
                                                           verbosity,
                                                           result,
                                                           "ClusterAggregate::runAggregate"sv));
            _extensionMetrics.markSuccess();
        }

        void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* reply) override {
            globalOpCounters().gotAggregate();

            if (_liteParsedPipeline.hasChangeStream()) {
                change_stream::recordCursorOptionMetrics(request().getCursor().getBatchSize(),
                                                         request().getMaxTimeMS());
            }

            const auto& body = unparsedRequest().body;
            CommandHelpers::handleMarkKillOnClientDisconnect(opCtx,
                                                             !Pipeline::aggHasWriteStage(body));

            // Run aggregate-specific semantic validation beyond what the IDL-parsing provides.
            aggregation_request_helper::validate(request(), body, ns(), opCtx->getClient());

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

            // Mark this request as 'explain' so that downstream components such as query stats key
            // construction can see it.
            request().setExplain(true);

            uassert(ErrorCodes::FailedToParse,
                    "The 'explain' option is illegal when an explain verbosity is also provided",
                    !unparsedRequest().body.hasField(AggregateCommandRequest::kExplainFieldName));

            // Run aggregate-specific semantic validation beyond what the IDL-parsing provides.
            aggregation_request_helper::validate(
                request(), unparsedRequest().body, ns(), opCtx->getClient());

            auto bodyBuilder = result->getBodyBuilder();
            _runAggCommand(opCtx, &bodyBuilder, verbosity);
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            Impl::doCheckAuthorization(opCtx, unparsedRequest(), _privileges);
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
     * Previously counted as a query (opcounters.queries). As of SERVER-123987, aggregate has its
     * own dedicated counter (opcounters.aggregates), incremented directly in run().
     */
    bool shouldAffectCommandCounter() const final {
        return false;
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
