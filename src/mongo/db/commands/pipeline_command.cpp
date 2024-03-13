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

#include <algorithm>
#include <boost/optional.hpp>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/node_hash_set.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/run_aggregate.h"
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
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/query/query_settings/query_settings_utils.h"
#include "mongo/db/read_concern_support_result.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/rpc/reply_builder_interface.h"
#include "mongo/s/sharding_state.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/serialization_context.h"

namespace mongo {
namespace {

class PipelineCommand final : public Command {
public:
    PipelineCommand() : Command("aggregate") {}

    const std::set<std::string>& apiVersions() const {
        return kApiVersions1;
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

        SerializationContext serializationCtx = opMsgRequest.getSerializationContext();

        const auto aggregationRequest = aggregation_request_helper::parseFromBSON(
            opCtx,
            opMsgRequest.getDbName(),
            opMsgRequest.body,
            explainVerbosity,
            APIParameters::get(opCtx).getAPIStrict().value_or(false),
            serializationCtx);

        auto privileges = uassertStatusOK(
            auth::getPrivilegesForAggregate(AuthorizationSession::get(opCtx->getClient()),
                                            aggregationRequest.getNamespace(),
                                            aggregationRequest,
                                            false));

        // TODO: SERVER-73632 Remove feature flag for PM-635.
        // Forbid users from passing 'querySettings' explicitly.
        uassert(7708001,
                "BSON field 'querySettings' is an unknown field",
                query_settings::utils::allowQuerySettingsFromClient(opCtx->getClient()) ||
                    !aggregationRequest.getQuerySettings().has_value());

        return std::make_unique<Invocation>(
            this, opMsgRequest, std::move(aggregationRequest), std::move(privileges));
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

    class Invocation final : public CommandInvocation {
    public:
        Invocation(Command* cmd,
                   const OpMsgRequest& request,
                   AggregateCommandRequest aggregationRequest,
                   PrivilegeVector privileges)
            : CommandInvocation(cmd),
              _request(request),
              _dbName(request.getDbName()),
              _aggregationRequest(std::move(aggregationRequest)),
              _liteParsedPipeline(_aggregationRequest),
              _privileges(std::move(privileges)) {
            auto externalDataSources = _aggregationRequest.getExternalDataSources();
            // Support collection-less aggregate commands without $_externalDataSources.
            if (_aggregationRequest.getNamespace().isCollectionlessAggregateNS()) {
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
                findCollNameInExternalDataSourceOption(_aggregationRequest.getNamespace().coll());
            uassert(7039003,
                    "Source namespace must be an external data source",
                    externalDataSourcesIter != externalDataSources->end());
            _usedExternalDataSources.emplace_back(_aggregationRequest.getNamespace(),
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

            if (auto&& pipeline = _aggregationRequest.getPipeline(); !pipeline.empty()) {
                // An external data source does not support writes and thus cannot be used as a
                // target for $merge / $out stages.
                auto&& lastStage = pipeline.back();
                uassert(7239302,
                        "The external data source cannot be used for $merge or $out stage",
                        !lastStage.hasField("$out"_sd) && !lastStage.hasField("$merge"_sd));
            }
        }

    private:
        bool supportsWriteConcern() const override {
            return true;
        }

        bool isSubjectToIngressAdmissionControl() const override {
            return true;
        }

        bool canIgnorePrepareConflicts() const override {
            // Aggregate is a special case for prepare conflicts. It may do writes to an output
            // collection, but it enables enforcement of prepare conflicts before doing so.
            return true;
        }

        ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level,
                                                     bool isImplicitDefault) const override {
            return _liteParsedPipeline.supportsReadConcern(
                level,
                isImplicitDefault,
                _aggregationRequest.getExplain(),
                serverGlobalParams.enableMajorityReadConcern);
        }

        bool allowsSpeculativeMajorityReads() const override {
            // Currently only change stream aggregation queries are allowed to use speculative
            // majority. The aggregation command itself will check this internally and fail if
            // necessary.
            return true;
        }

        void run(OperationContext* opCtx, rpc::ReplyBuilderInterface* reply) override {
            CommandHelpers::handleMarkKillOnClientDisconnect(
                opCtx, !Pipeline::aggHasWriteStage(_request.body));

            uassertStatusOK(runAggregate(opCtx,
                                         _aggregationRequest,
                                         _liteParsedPipeline,
                                         _request.body,
                                         _privileges,
                                         reply,
                                         _usedExternalDataSources));

            // The aggregate command's response is unstable when 'explain' or 'exchange' fields are
            // set.
            if (!_aggregationRequest.getExplain() && !_aggregationRequest.getExchange()) {
                query_request_helper::validateCursorResponse(
                    reply->getBodyBuilder().asTempObj(),
                    auth::ValidatedTenancyScope::get(opCtx),
                    _aggregationRequest.getNamespace().tenantId(),
                    _aggregationRequest.getSerializationContext());
            }
        }

        NamespaceString ns() const override {
            return _aggregationRequest.getNamespace();
        }

        void explain(OperationContext* opCtx,
                     ExplainOptions::Verbosity verbosity,
                     rpc::ReplyBuilderInterface* result) override {
            // See run() method for details.
            uassertStatusOK(runAggregate(opCtx,
                                         _aggregationRequest,
                                         _liteParsedPipeline,
                                         _request.body,
                                         _privileges,
                                         result,
                                         _usedExternalDataSources));
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForPrivileges(_privileges));
        }

        const OpMsgRequest& _request;
        const DatabaseName _dbName;
        AggregateCommandRequest _aggregationRequest;
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
    ReadWriteType getReadWriteType() const {
        return ReadWriteType::kRead;
    }

    const AuthorizationContract* getAuthorizationContract() const final {
        return &::mongo::AggregateCommandRequest::kAuthorizationContract;
    }

    bool allowedInTransactions() const final {
        return true;
    }
};
MONGO_REGISTER_COMMAND(PipelineCommand).forShard();

}  // namespace
}  // namespace mongo
