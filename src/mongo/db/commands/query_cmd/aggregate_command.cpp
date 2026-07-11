// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/change_stream_metrics_util.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/query_cmd/extension_metrics.h"
#include "mongo/db/commands/query_cmd/run_aggregate.h"
#include "mongo/db/database_name.h"
#include "mongo/db/database_name_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/external_data_source_option_gen.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/explain_verbosity_gen.h"
#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/read_concern_support_result.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/serialization_context.h"

#include <algorithm>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

class AggregateCommand final : public TypedCommand<AggregateCommand> {
public:
    using Request = AggregateCommandRequest;

    AggregateCommand() : TypedCommand(Request::kCommandName) {}

    const std::set<std::string>& apiVersions() const override {
        return kApiVersions1;
    }

    bool allowedWithSecurityToken() const final {
        return true;
    }

    /**
     * As of SERVER-123987, aggregate has its own dedicated counter (opcounters.aggregates),
     * incremented directly in run(). Before that it was counted as a query (opcounters.queries).
     */
    bool shouldAffectCommandCounter() const override {
        return false;
    }

    bool shouldAffectReadOptionCounters() const override {
        return true;
    }

    bool collectsResourceConsumptionMetrics() const override {
        return true;
    }

    bool enableDiagnosticPrintingOnFailure() const final {
        return true;
    }

    bool supportsQuerySettings() const override {
        return true;
    }

    class Invocation final : public MinimalInvocationBase {
    public:
        Invocation(OperationContext* opCtx, Command* cmd, const OpMsgRequest& opMsgRequest)
            : MinimalInvocationBase(opCtx, cmd, opMsgRequest),
              _ifrContext([&]() {
                  // Build the IFR context based on how this aggregate arrived, distinguishing
                  // "no request flag values from the router" from "no request flag values because
                  // this is a standalone/direct request":
                  //   - Flag values are present: use them. (An older router may omit newer flags;
                  //     IncrementalFeatureRolloutContext treats those omitted serialize-on-outgoing
                  //     flags as disabled rather than consulting this node's FCV.)
                  //   - No flag values but the request came from a router: the router did not
                  //     serialize any flags, so there are *implied* flag values -- all
                  //     serialize-on-outgoing flags should be treated as disabled. Build from an
                  //     empty flag set so those implied values are applied instead of falling back
                  //     to this node's local FCV/state. See the IFRContext constructors for more
                  //     detail.
                  //   - No flag values and not from a router (standalone mongod / direct client):
                  //     start from scratch and resolve each flag against this node's local state.
                  const auto& requestFlagValues = request().getIfrFlags();
                  if (requestFlagValues.has_value()) {
                      return std::make_shared<IncrementalFeatureRolloutContext>(
                          requestFlagValues.value());
                  }

                  if (aggregation_request_helper::getFromRouter(request()).value_or(false)) {
                      return std::make_shared<IncrementalFeatureRolloutContext>(
                          std::span<const BSONObj>{});
                  }

                  return std::make_shared<IncrementalFeatureRolloutContext>();
              }()),
              _extensionMetrics(
                  static_cast<const AggregateCommand*>(cmd)->getExtensionMetricsAllocation()),
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
                                                  false))) {
            if (auto pipelineForLog = _liteParsedPipeline.pipelineToBsonForLog()) {
                aggregation_request_helper::updateOpDescriptionForLog(
                    opCtx, unparsedRequest().body, *pipelineForLog);
            }
            auto externalDataSources = request().getExternalDataSources();
            // Support collection-less aggregate commands without $_externalDataSources.
            if (request().getNamespace().isCollectionlessAggregateNS()) {
                uassert(7604400,
                        "$_externalDataSources can't be used with the collectionless aggregate",
                        !externalDataSources.has_value());
                return;
            }

            uassert(7039000,
                    "Either $_externalDataSources must always be present when enableComputeMode="
                    "true or must not when enableComputeMode=false",
                    computeModeEnabled == externalDataSources.has_value());

            if (!externalDataSources) {
                return;
            }
            uassert(7039002,
                    "Expected one or more external data source but got 0",
                    externalDataSources->size() > 0);

            for (auto&& option : *externalDataSources) {
                uassert(7039001,
                        "Expected one or more urls for an external data source but got 0",
                        option.getDataSources().size() > 0);
            }

            auto findCollNameInExternalDataSourceOption = [&](std::string_view collName) {
                return std::find_if(externalDataSources->begin(),
                                    externalDataSources->end(),
                                    [&](const ExternalDataSourceOption& externalDataSourceOption) {
                                        return externalDataSourceOption.getCollName() == collName;
                                    });
            };

            auto externalDataSourcesIter =
                findCollNameInExternalDataSourceOption(request().getNamespace().coll());
            uassert(7039003,
                    "Source namespace must be an external data source",
                    externalDataSourcesIter != externalDataSources->end());
            _usedExternalDataSources.emplace_back(request().getNamespace(),
                                                  externalDataSourcesIter->getDataSources());

            for (const auto& involvedNamespace : _liteParsedPipeline.getInvolvedNamespaces()) {
                externalDataSourcesIter =
                    findCollNameInExternalDataSourceOption(involvedNamespace.coll());
                uassert(7039004,
                        "Involved namespace must be an external data source",
                        externalDataSourcesIter != externalDataSources->end());
                _usedExternalDataSources.emplace_back(involvedNamespace,
                                                      externalDataSourcesIter->getDataSources());
            }

            uassert(7239302,
                    "The external data source cannot be used for write operations",
                    isReadOperation());
        }

        bool isReadOperation() const override {
            // Only checks for the last stage since currently write stages are only allowed to be at
            // the end of the pipeline.
            return !_liteParsedPipeline.endsWithWriteStage();
        }

        const GenericArguments& getGenericArguments() const override {
            return request().getGenericArguments();
        }

    private:
        bool supportsWriteConcern() const override {
            return true;
        }

        bool isSubjectToIngressAdmissionControl() const override {
            return !_liteParsedPipeline.isExemptFromIngressAdmissionControl();
        }

        bool canIgnorePrepareConflicts() const override {
            // Aggregate is a special case for prepare conflicts. It may do writes to an output
            // collection, but it enables enforcement of prepare conflicts before doing so.
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

        bool allowsSpeculativeMajorityReads() const override {
            // Currently only change stream aggregation queries are allowed to use speculative
            // majority. The aggregation command itself will check this internally and fail if
            // necessary.
            return true;
        }

        void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* reply) override {
            globalOpCounters().gotAggregate();

            if (_liteParsedPipeline.hasChangeStream()) {
                change_stream::recordCursorOptionMetrics(request().getCursor().getBatchSize(),
                                                         request().getMaxTimeMS());
            }

            const auto& explain = request().getExplain();
            const auto& body = unparsedRequest().body;
            boost::optional<ExplainOptions::Verbosity> verbosity = boost::none;

            // Run aggregate-specific semantic validation beyond what the IDL-parsing provides.
            aggregation_request_helper::validate(request(), body, ns(), opCtx->getClient());
            CommandHelpers::handleMarkKillOnClientDisconnect(opCtx,
                                                             !Pipeline::aggHasWriteStage(body));


            // If aggregation contains inline 'explain' we set explain verbosity.
            if (explain.get_value_or(false)) {
                verbosity = ExplainOptions::Verbosity::kQueryPlanner;
            }

            // We disallow deprioritization of the applying phase of resharding to ensure that the
            // operation does not time out during the critical section.
            // TODO (SERVER-122847): Remove this code.
            const bool isOplogNss = (ns() == NamespaceString::kRsOplogNamespace);
            boost::optional<admission::execution_control::ScopedTaskTypeNonDeprioritizable>
                nonDeprioMarker;
            if (!gExecutionControlRemoteSpecification.isEnabledUseLastLTSFCVWhenUninitialized(
                    VersionContext::getDecoration(opCtx),
                    serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) &&
                isOplogNss && opCtx->getClient()->isInternalClient()) {
                nonDeprioMarker.emplace(opCtx);
            }

            // TODO(CLOUDP-319941): Remove this when atlas uses the priority port for monitoring
            // operations
            if (_liteParsedPipeline.startsWithCurrentOpStage() &&
                !opCtx->inMultiDocumentTransaction() && !nonDeprioMarker) {
                nonDeprioMarker.emplace(opCtx);
            }

            uassertStatusOK(runAggregate(opCtx,
                                         request(),
                                         _liteParsedPipeline,
                                         body,
                                         _privileges,
                                         verbosity,
                                         reply,
                                         _usedExternalDataSources,
                                         _ifrContext));

            // The aggregate command's response is unstable when 'explain' or 'exchange' fields are
            // set.
            if (!explain && !request().getExchange()) {
                query_request_helper::validateCursorResponse(
                    reply->getBodyBuilder().asTempObj(),
                    auth::ValidatedTenancyScope::get(opCtx),
                    ns().tenantId(),
                    request().getSerializationContext());
            }
            _extensionMetrics.markSuccess();
        }

        void explain(OperationContext* opCtx,
                     ExplainOptions::Verbosity verbosity,
                     rpc::ReplyBuilderInterface* reply) override {
            const auto& body = unparsedRequest().body;

            // Mark this request as 'explain' so that downstream components such as query stats key
            // construction can see it.
            request().setExplain(true);

            uassert(ErrorCodes::FailedToParse,
                    "The 'explain' option is illegal when an explain verbosity is also provided",
                    !body.hasField(AggregateCommandRequest::kExplainFieldName));

            aggregation_request_helper::validate(request(), body, ns(), opCtx->getClient());

            // See run() method for details.
            uassertStatusOK(runAggregate(opCtx,
                                         request(),
                                         _liteParsedPipeline,
                                         body,
                                         _privileges,
                                         verbosity,
                                         reply,
                                         _usedExternalDataSources,
                                         _ifrContext));
        }

        bool canRetryOnStaleShardMetadataError(const OpMsgRequest& /* unused */) const override {
            // Can not rerun the command when executing an aggregation that runs $mergeCursors as it
            // may have consumed the cursors within.
            return !aggregation_request_helper::hasMergeCursors(request());
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForPrivileges(_privileges));
        }

        NamespaceString ns() const override {
            return request().getNamespace();
        }

        // Store the IFR context as a shared pointer. This object is mutable while running the
        // command. It must be shared because commands with subpipelines pass the same IFR context
        // in to child subpipeline expression contexts, rather than copying it. We want to ensure
        // that a consistent and single IFR context is used across the entirety of query processing,
        // including in child subpipelines.
        std::shared_ptr<IncrementalFeatureRolloutContext> _ifrContext;
        ExtensionMetrics _extensionMetrics;
        const LiteParsedPipeline _liteParsedPipeline;
        const PrivilegeVector _privileges;
        std::vector<std::pair<NamespaceString, std::vector<ExternalDataSourceInfo>>>
            _usedExternalDataSources;
    };

    std::string help() const override {
        return "Runs the aggregation command. See http://dochub.mongodb.org/core/aggregation for "
               "more details.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kOptIn;
    }
    bool maintenanceOk() const override {
        return false;
    }
    ReadWriteType getReadWriteType() const override {
        return ReadWriteType::kRead;
    }

    const AuthorizationContract* getAuthorizationContract() const final {
        return &::mongo::AggregateCommandRequest::kAuthorizationContract;
    }

    bool allowedInTransactions() const final {
        return true;
    }

    const ExtensionMetricsAllocation& getExtensionMetricsAllocation() const {
        tassert(
            11695901,
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
MONGO_REGISTER_COMMAND(AggregateCommand).forShard();

}  // namespace
}  // namespace mongo
