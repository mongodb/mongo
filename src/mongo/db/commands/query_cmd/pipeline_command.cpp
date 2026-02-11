/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/query_cmd/extension_metrics.h"
#include "mongo/db/commands/query_cmd/run_aggregate.h"
#include "mongo/db/database_name.h"
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
#include "mongo/db/query/query_settings/query_settings_service.h"
#include "mongo/db/read_concern_support_result.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/serialization_context.h"

#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

class PipelineCommand final : public TypedCommand<PipelineCommand> {
public:
    using Request = AggregateCommandRequest;

    PipelineCommand() : TypedCommand(Request::kCommandName) {}

    const std::set<std::string>& apiVersions() const override {
        return kApiVersions1;
    }

    bool allowedWithSecurityToken() const final {
        return true;
    }

    /**
     * A pipeline/aggregation command does not increment the command counter, but rather increments
     * the query counter.
     */
    bool shouldAffectCommandCounter() const override {
        return false;
    }

    bool shouldAffectQueryCounter() const override {
        return true;
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

    class Invocation final : public MinimalInvocationBase {
    public:
        Invocation(OperationContext* opCtx, Command* cmd, const OpMsgRequest& opMsgRequest)
            : MinimalInvocationBase(opCtx, cmd, opMsgRequest),
              _ifrContext([&]() {
                  const auto& requestFlagValues = request().getIfrFlags();
                  return requestFlagValues.has_value()
                      ? std::make_shared<IncrementalFeatureRolloutContext>(
                            requestFlagValues.value())
                      : std::make_shared<IncrementalFeatureRolloutContext>();
              }()),
              _extensionMetrics(
                  static_cast<const PipelineCommand*>(cmd)->getExtensionMetricsAllocation()),
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

            auto findCollNameInExternalDataSourceOption = [&](StringData collName) {
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
            const auto& explain = request().getExplain();
            const auto& body = unparsedRequest().body;
            boost::optional<ExplainOptions::Verbosity> verbosity = boost::none;

            uassertNoQuerySettings(opCtx);

            // Run aggregate-specific semantic validation beyond what the IDL-parsing provides. We
            // pass boost::none as explainVerbosity because 'validate()' interprets a non-none
            // explainVerbosity as a top-level explain.
            // TODO SERVER-119402: Change explainVerbosity parameter to bool.
            aggregation_request_helper::validate(request(), body, ns(), boost::none);
            CommandHelpers::handleMarkKillOnClientDisconnect(opCtx,
                                                             !Pipeline::aggHasWriteStage(body));


            // If aggregation contains inline 'explain' we set explain verbosity.
            if (explain.get_value_or(false)) {
                verbosity = ExplainOptions::Verbosity::kQueryPlanner;
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

            uassertNoQuerySettings(opCtx);

            // See run() for why we need this validation.
            // TODO SERVER-119402: Change explainVerbosity parameter to bool.
            aggregation_request_helper::validate(request(), body, ns(), verbosity);

            // Mark this request as 'explain' so that downstream components such as query stats key
            // construction can see it.
            request().setExplain(true);

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

        void uassertNoQuerySettings(OperationContext* opCtx) const {
            // Forbid users from passing 'querySettings' explicitly.
            uassert(7708001,
                    "BSON field 'querySettings' is an unknown field",
                    query_settings::allowQuerySettingsFromClient(opCtx->getClient()) ||
                        !request().getQuerySettings().has_value());
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
MONGO_REGISTER_COMMAND(PipelineCommand).forShard();

}  // namespace
}  // namespace mongo
