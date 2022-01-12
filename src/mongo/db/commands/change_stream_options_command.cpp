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
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/logv2/log.h"

namespace mongo {
namespace {
/**
 * A versioned command class to set the change streams options. This command should not run
 * on 'mongoS'.
 */
class SetChangeStreamOptionsCommand
    : public SetChangeStreamOptionsCmdVersion1Gen<SetChangeStreamOptionsCommand> {
public:
    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    bool adminOnly() const final {
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
            assertCanIssueCommand(opCtx);
            return setOptionsAndReply(opCtx);
        }

    private:
        /**
         * A helper to verify if the command can be run. Throws 'uassert' in case of failures.
         */
        void assertCanIssueCommand(OperationContext* opCtx) {
            const auto replCoord = repl::ReplicationCoordinator::get(opCtx);

            uassert(5869200,
                    str::stream() << "'" << SetChangeStreamOptions::kCommandName
                                  << "' is not supported on standalone nodes.",
                    replCoord->isReplEnabled());

            uassert(5869201,
                    str::stream() << "'" << SetChangeStreamOptions::kCommandName
                                  << "' is not supported on shard nodes.",
                    serverGlobalParams.clusterRole != ClusterRole::ShardServer);

            uassert(5869202,
                    "Expected at least one change stream option to set",
                    request().getPreAndPostImages());
        }

        /**
         * Sets the change streams options using 'changeStreamOptionsManager'.
         */
        Reply setOptionsAndReply(OperationContext* opCtx) {
            // Create a request object for the 'changeStreamOptionsManager'.
            auto optionsToSet = ChangeStreamOptions();

            if (auto& preAndPostImage = request().getPreAndPostImages()) {
                uassert(5869203,
                        "Expected 'expireAfterSeconds' option",
                        preAndPostImage->getExpireAfterSeconds());

                stdx::visit(
                    visit_helper::Overloaded{
                        [&](const std::string& expirationPolicyMode) {
                            uassert(5869204,
                                    "Non-numeric value of 'expireAfterSeconds' should be 'off'",
                                    expirationPolicyMode == "off");
                        },
                        [&](const std::int64_t& expiryTime) {
                            uassert(5869205,
                                    "Numeric value of 'expireAfterSeconds' should be positive",
                                    expiryTime > 0);
                        }},
                    *preAndPostImage->getExpireAfterSeconds());

                optionsToSet.setPreAndPostImages(preAndPostImage);
            }

            // Store the change streams configuration options.
            auto& changeStreamOptionManager = ChangeStreamOptionsManager::get(opCtx);
            auto status = changeStreamOptionManager.setOptions(opCtx, optionsToSet);
            uassert(5869206,
                    str::stream() << "Failed to set change stream options, status: "
                                  << status.getStatus().codeString(),
                    status.isOK());

            return Reply();
        }

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
} setChangeStreamOptionsCommand;

/**
 * A versioned command class to get the change streams options. This command should not run on
 * 'mongoS'.
 */
class GetChangeStreamOptionsCommand
    : public GetChangeStreamOptionsCmdVersion1Gen<GetChangeStreamOptionsCommand> {
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
            assertCanIssueCommand(opCtx);
            return getOptionsAndReply(opCtx);
        }

    private:
        /**
         * A helper to verify if the command can be run. Throws 'uassert' in case of failures.
         */
        void assertCanIssueCommand(OperationContext* opCtx) {
            const auto replCoord = repl::ReplicationCoordinator::get(opCtx);

            uassert(5869207,
                    str::stream() << "'" << GetChangeStreamOptions::kCommandName
                                  << "' is not supported on standalone nodes.",
                    replCoord->isReplEnabled());

            uassert(5869208,
                    str::stream() << "'" << GetChangeStreamOptions::kCommandName
                                  << "' is not supported on shard nodes.",
                    serverGlobalParams.clusterRole != ClusterRole::ShardServer);
        }

        /**
         * Gets the change streams options from the 'ChangeStreamOptionsManager', creates a response
         * from it, and return it back to the client.
         */
        Reply getOptionsAndReply(OperationContext* opCtx) {
            auto reply = Reply();

            // Get the change streams options, if present and return it back to the client.
            auto& changeStreamOptionManager = ChangeStreamOptionsManager::get(opCtx);

            if (auto changeStreamOptions = changeStreamOptionManager.getOptions(opCtx)) {
                if (auto preAndPostImages = changeStreamOptions->getPreAndPostImages()) {
                    // Add 'expiredAfterSeconds' to the reply message only when the default
                    // expiration policy is not enabled. A string type of 'expireAfterSeconds'
                    // signifies that the value it holds is 'off'.
                    if (preAndPostImages->getExpireAfterSeconds() &&
                        !stdx::holds_alternative<std::string>(
                            *preAndPostImages->getExpireAfterSeconds())) {
                        reply.setChangeStreamOptions(std::move(*changeStreamOptions));
                    }
                }
            }

            return reply;
        }

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
} getChangeStreamOptionsCommand;

}  // namespace
}  // namespace mongo
