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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/auth/authorization_checks.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/change_stream_options_manager.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/change_stream_options_gen.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/logv2/log.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"

namespace mongo {
namespace {

/**
 * Creates a reply object of type 'Reply' from raw 'response' BSON object. Any extra fields are
 * removed from the 'response' object and then passed into to the IDL parser to create the response
 * object.
 */
template <typename Reply>
Reply createReply(BSONObj response, const std::string& errorContextField) {
    constexpr auto kReplData = "$replData"_sd;
    constexpr auto kLastCommittedOpTime = "lastCommittedOpTime"_sd;
    constexpr auto kClusterTime = "$clusterTime"_sd;
    constexpr auto kConfigTime = "$configTime"_sd;
    constexpr auto kTopologyTime = "$topologyTime"_sd;
    constexpr auto kOperationTime = "operationTime"_sd;

    // A set of fields to be removed from the 'response' object.
    StringDataSet ignorableFields({kReplData,
                                   kLastCommittedOpTime,
                                   ErrorReply::kOkFieldName,
                                   kClusterTime,
                                   kConfigTime,
                                   kTopologyTime,
                                   kOperationTime});

    // Create the reply object from the redacted response object.
    return Reply::parse(IDLParserErrorContext(errorContextField),
                        response.removeFields(ignorableFields));
}

class ClusterSetChangeStreamOptionsCommand
    : public SetChangeStreamOptionsCmdVersion1Gen<ClusterSetChangeStreamOptionsCommand> {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Sets change streams configuration options.\n"
               "Usage: { setChangeStreamOptions: 1, preAndPostImages: { expireAfterSeconds: "
               "<long>|'off'> }, writeConcern: { <write concern> }}";
    }

    class Invocation final : public InvocationBaseGen {
    public:
        Invocation(OperationContext* opCtx,
                   const Command* command,
                   const OpMsgRequest& opMsgRequest)
            : InvocationBaseGen(opCtx, command, opMsgRequest) {}

        bool supportsWriteConcern() const final {
            return true;
        }

        NamespaceString ns() const final {
            return NamespaceString();
        }

        Reply typedRun(OperationContext* opCtx) final {
            // Get the configuration server connection and dispatch the request to it to set the
            // change streams options.
            auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
            auto cmdResponse = uassertStatusOK(configShard->runCommandWithFixedRetryAttempts(
                opCtx,
                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                NamespaceString::kAdminDb.toString(),
                CommandHelpers::appendMajorityWriteConcern(
                    CommandHelpers::filterCommandRequestForPassthrough(request().toBSON({})),
                    opCtx->getWriteConcern()),
                Shard::RetryPolicy::kNotIdempotent));

            uassertStatusOK(cmdResponse.commandStatus);

            return Reply();
        }

    private:
        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Feature flag featureFlagChangeStreamPreAndPostImagesTimeBasedRetentionPolicy "
                    "must be enabled",
                    feature_flags::gFeatureFlagChangeStreamPreAndPostImagesTimeBasedRetentionPolicy
                        .isEnabled(serverGlobalParams.featureCompatibility));

            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForPrivilege(Privilege{ResourcePattern::forClusterResource(),
                                                             ActionType::setChangeStreamOptions}));
        }
    };
} clusterSetChangeStreamOptionsCommand;

class ClusterGetChangeStreamOptionsCommand
    : public GetChangeStreamOptionsCmdVersion1Gen<ClusterGetChangeStreamOptionsCommand> {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }
    bool adminOnly() const override {
        return true;
    }

    std::string help() const final {
        return "Gets change streams configuration options.\n"
               "Usage: { getChangeStreamOptions: 1 }";
    }

    class Invocation final : public InvocationBaseGen {
    public:
        Invocation(OperationContext* opCtx,
                   const Command* command,
                   const OpMsgRequest& opMsgRequest)
            : InvocationBaseGen(opCtx, command, opMsgRequest) {}

        bool supportsWriteConcern() const final {
            return false;
        }

        NamespaceString ns() const final {
            return NamespaceString();
        }

        Reply typedRun(OperationContext* opCtx) final {
            // Get the change streams options from the configuration server.
            auto configShard = Grid::get(opCtx)->shardRegistry()->getConfigShard();
            auto cmdResponse = uassertStatusOK(configShard->runCommandWithFixedRetryAttempts(
                opCtx,
                ReadPreferenceSetting(ReadPreference::PrimaryOnly),
                NamespaceString::kAdminDb.toString(),
                applyReadWriteConcern(
                    opCtx,
                    this,
                    CommandHelpers::filterCommandRequestForPassthrough(request().toBSON({}))),
                Shard::RetryPolicy::kIdempotent));

            uassertStatusOK(cmdResponse.commandStatus);

            // Create the reply object from the configuration server response and return it back to
            // the client.
            return createReply<Reply>(cmdResponse.response, "ClusterGetChangeStreamOptionsCommand");
        }

    private:
        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Feature flag featureFlagChangeStreamPreAndPostImagesTimeBasedRetentionPolicy "
                    "must be enabled",
                    feature_flags::gFeatureFlagChangeStreamPreAndPostImagesTimeBasedRetentionPolicy
                        .isEnabled(serverGlobalParams.featureCompatibility));

            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForPrivilege(Privilege{ResourcePattern::forClusterResource(),
                                                             ActionType::getChangeStreamOptions}));
        }
    };
} clusterGetChangeStreamOptionsCommand;

}  // namespace
}  // namespace mongo
