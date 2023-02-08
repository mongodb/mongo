/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/s/move_primary/move_primary_recipient_cmds_gen.h"
#include "mongo/db/s/move_primary/move_primary_recipient_service.h"
#include "mongo/db/s/move_primary/move_primary_state_machine_gen.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/util/assert_util.h"

namespace mongo {

namespace {

class MovePrimaryRecipientSyncDataCmd : public TypedCommand<MovePrimaryRecipientSyncDataCmd> {
public:
    using Request = MovePrimaryRecipientSyncData;

    class Invocation : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            // (Generic FCV reference): This FCV reference should exist across LTS binary versions.
            uassert(7249200,
                    "movePrimaryRecipientSyncData not available while upgrading or downgrading the "
                    "recipient FCV",
                    !serverGlobalParams.featureCompatibility.isUpgradingOrDowngrading());

            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            const auto& cmd = request();
            MovePrimaryRecipientDocument recipientDoc(std::move(cmd.getMigrationId()),
                                                      cmd.getDatabaseName().toString(),
                                                      cmd.getFromShard().toString());

            auto registry = repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext());
            auto service = registry->lookupServiceByName(
                MovePrimaryRecipientService::kMovePrimaryRecipientServiceName);
            auto instance = MovePrimaryRecipientService::MovePrimaryRecipient::getOrCreate(
                opCtx, service, recipientDoc.toBSON());

            auto returnAfterReachingDonorTimestamp = cmd.getReturnAfterReachingDonorTimestamp();

            if (!returnAfterReachingDonorTimestamp) {
                return instance->waitUntilMigrationReachesConsistentState(opCtx);
            }

            return instance->waitUntilMigrationReachesReturnAfterReachingDonorTimestamp(
                opCtx, returnAfterReachingDonorTimestamp.get());
        }

    private:
        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::internal));
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName(), "");
        }
    };

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command sent by the movePrimary operation donor to the recipient to sync "
               "data with the donor.";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext* context) const override {
        return Command::AllowedOnSecondary::kNever;
    }
} movePrimaryRecipientSyncDataCmd;

class MovePrimaryRecipientForgetMigrationCmd
    : public TypedCommand<MovePrimaryRecipientForgetMigrationCmd> {
public:
    using Request = MovePrimaryRecipientForgetMigration;

    class Invocation : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            const auto& cmd = request();

            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            auto& migrationId = cmd.getMigrationId();
            auto registry = repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext());
            auto service = registry->lookupServiceByName(
                MovePrimaryRecipientService::kMovePrimaryRecipientServiceName);

            auto instance = MovePrimaryRecipientService::MovePrimaryRecipient::lookup(
                opCtx, service, migrationId.toBSON());
            if (instance) {
                instance->get()->onReceiveRecipientForgetMigration(opCtx);
                return instance->get()->getForgetMigrationDurableFuture().get(opCtx);
            }
            uassert(
                ErrorCodes::MovePrimaryRecipientDocNotFound,
                "Received movePrimaryRecipientForgetMigration for a non-existent state document",
                instance);
        }

    private:
        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::internal));
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName(), "");
        }
    };

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    std::string help() const override {
        return "Internal command sent by the movePrimary operation donor to mark state doc garbage"
               " collectable after a successful data sync.";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext* context) const override {
        return Command::AllowedOnSecondary::kNever;
    }
} movePrimaryRecipientForgetMigrationCmd;

class MovePrimaryRecipientAbortMigrationCmd
    : public TypedCommand<MovePrimaryRecipientAbortMigrationCmd> {
public:
    using Request = MovePrimaryRecipientAbortMigration;

    class Invocation : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            const auto& cmd = request();

            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            // It is possible that the movePrimaryRecipientAbortMigration cmd is received before
            // movePrimaryRecipientSyncData if movePrimaryDonorAbortMigration is called by the
            // coordinator before the recipient receives movePrimaryRecipientSyncData from the
            // donor. We create a new state doc and set its state to kAborted to disallow migration
            // from starting when movePrimaryRecipientSyncData reaches the recipient.
            MovePrimaryRecipientDocument recipientDoc(std::move(cmd.getMigrationId()),
                                                      cmd.getDatabaseName().toString(),
                                                      cmd.getFromShard().toString());
            recipientDoc.setState(MovePrimaryRecipientState::kAborted);

            auto registry = repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext());
            auto service = registry->lookupServiceByName(
                MovePrimaryRecipientService::kMovePrimaryRecipientServiceName);
            auto instance = MovePrimaryRecipientService::MovePrimaryRecipient::getOrCreate(
                opCtx, service, recipientDoc.toBSON());

            instance->onRecieveRecipientAbortMigration(opCtx);
            instance->getAbortMigrationDurableFuture().get(opCtx);
        }

    private:
        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::internal));
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName(), "");
        }
    };

    bool skipApiVersionCheck() const override {
        // Internal command (server to server).
        return true;
    }

    bool adminOnly() const override {
        return true;
    }

    std::string help() const override {
        return "Internal command sent by the movePrimary operation donor to abort the movePrimary "
               "operation at the recipient.";
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext* context) const override {
        return Command::AllowedOnSecondary::kNever;
    }
} movePrimaryRecipientAbortMigrationCmd;

}  // namespace

}  // namespace mongo
