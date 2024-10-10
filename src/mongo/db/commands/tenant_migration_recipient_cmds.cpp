/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <memory>
#include <set>
#include <string>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/tenant_migration_recipient_cmds_gen.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/tenant_migration_recipient_service.h"
#include "mongo/db/repl/tenant_migration_state_machine_gen.h"
#include "mongo/db/repl/tenant_migration_util.h"
#include "mongo/db/server_options.h"
#include "mongo/db/serverless/serverless_types_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/tenant_id.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(returnResponseOkForRecipientSyncDataCmd);
MONGO_FAIL_POINT_DEFINE(returnResponseOkForRecipientForgetMigrationCmd);

class RecipientSyncDataCmd : public TypedCommand<RecipientSyncDataCmd> {
public:
    using Request = RecipientSyncData;
    using Response = RecipientSyncDataResponse;

    class Invocation : public InvocationBase {

    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::IllegalOperation,
                    "tenant migrations are not available on config servers",
                    !serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));

            uassert(ErrorCodes::IllegalOperation,
                    "tenant migrations are only available if --serverless is enabled",
                    repl::ReplicationCoordinator::get(opCtx)->getSettings().isServerless());

            uassert(ErrorCodes::IllegalOperation,
                    "Cannot run tenant migration with x509 authentication",
                    repl::tenantMigrationDisableX509Auth);

            // (Generic FCV reference): This FCV reference should exist across LTS binary versions.
            uassert(
                5356101,
                "recipientSyncData not available while upgrading or downgrading the recipient FCV",
                !serverGlobalParams.featureCompatibility.acquireFCVSnapshot()
                     .isUpgradingOrDowngrading());

            const auto& cmd = request();
            const auto migrationProtocol = cmd.getProtocol().value_or(kDefaultMigrationProtocol);
            const auto& tenantId = cmd.getTenantId();
            const auto& tenantIds = cmd.getTenantIds();

            tenant_migration_util::protocolTenantIdCompatibilityCheck(migrationProtocol, tenantId);
            tenant_migration_util::protocolTenantIdsCompatibilityCheck(migrationProtocol,
                                                                       tenantIds);
            tenant_migration_util::protocolStorageOptionsCompatibilityCheck(opCtx,
                                                                            migrationProtocol);
            tenant_migration_util::protocolReadPreferenceCompatibilityCheck(
                opCtx, migrationProtocol, cmd.getReadPreferenceSettings());

            if (MONGO_unlikely(returnResponseOkForRecipientSyncDataCmd.shouldFail())) {
                LOGV2(4879608,
                      "Immediately returning OK because failpoint is enabled.",
                      "migrationId"_attr = cmd.getMigrationId(),
                      "fpName"_attr = returnResponseOkForRecipientSyncDataCmd.getName());
                return Response(repl::OpTime());
            }

            switch (migrationProtocol) {
                case MigrationProtocolEnum::kMultitenantMigrations:
                    return _handleMTMRecipientSyncDataCmd(opCtx, cmd);
                default:
                    MONGO_UNREACHABLE;
            }

            MONGO_UNREACHABLE;
        }

    private:
        Response _handleMTMRecipientSyncDataCmd(OperationContext* opCtx, const Request& cmd) {
            TenantMigrationRecipientDocument stateDoc(cmd.getMigrationId(),
                                                      cmd.getDonorConnectionString().toString(),
                                                      cmd.getTenantId()->toString(),
                                                      cmd.getStartMigrationDonorTimestamp(),
                                                      cmd.getReadPreferenceSettings());
            stateDoc.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);

            auto recipientService =
                repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext())
                    ->lookupServiceByName(repl::TenantMigrationRecipientService::
                                              kTenantMigrationRecipientServiceName);
            auto recipientInstance = repl::TenantMigrationRecipientService::Instance::getOrCreate(
                opCtx, recipientService, stateDoc.toBSON());

            auto returnAfterReachingDonorTs = cmd.getReturnAfterReachingDonorTimestamp();

            return returnAfterReachingDonorTs
                ? Response(recipientInstance->waitUntilMigrationReachesReturnAfterReachingTimestamp(
                      opCtx, *returnAfterReachingDonorTs))
                : Response(recipientInstance->waitUntilMigrationReachesConsistentState(opCtx));
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::runTenantMigration));
        }

        bool supportsWriteConcern() const override {
            return false;
        }
        NamespaceString ns() const override {
            return NamespaceString(request().getDbName());
        }
    };

    std::string help() const override {
        return "Internal replica set command; instructs the recipient to sync data as part of a "
               "tenant migration.";
    }

    bool adminOnly() const override {
        return true;
    }

    BasicCommand::AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return BasicCommand::AllowedOnSecondary::kNever;
    }
};
MONGO_REGISTER_COMMAND(RecipientSyncDataCmd).forShard();

class RecipientVoteImportedFilesCommand final
    : public TypedCommand<RecipientVoteImportedFilesCommand> {
public:
    using Request = RecipientVoteImportedFiles;

    std::string help() const override {
        return "An internal mongod command to track which members have imported all donated files"
               " during a tenant migration";
    }

    bool adminOnly() const override {
        return true;
    }

    AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return AllowedOnSecondary::kNever;
    }

    class Invocation final : public InvocationBase {
    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            BSONObjBuilder result;
            uassertStatusOK(
                repl::ReplicationCoordinator::get(opCtx)->checkReplEnabledForCommand(&result));

            uassert(ErrorCodes::IllegalOperation,
                    "tenant migrations are only available if --serverless is enabled",
                    repl::ReplicationCoordinator::get(opCtx)->getSettings().isServerless());

            const auto& cmd = request();
            LOGV2(6112805,
                  "Received RecipientVoteImportedFiles request",
                  "migrationId"_attr = cmd.getMigrationId(),
                  "from"_attr = cmd.getFrom());
        }

    private:
        NamespaceString ns() const override {
            return NamespaceString(request().getDbName());
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        void doCheckAuthorization(OperationContext* opCtx) const override {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::internal));
        }
    };
};
MONGO_REGISTER_COMMAND(RecipientVoteImportedFilesCommand).forShard();

class RecipientForgetMigrationCmd : public TypedCommand<RecipientForgetMigrationCmd> {
public:
    using Request = RecipientForgetMigration;

    // We may not have a document if recipientForgetMigration is received before
    // recipientSyncData. But even if that's the case, we still need to create an instance
    // and persist a state document that's marked garbage collectable (which is done by the
    // main chain).
    static inline const Timestamp kUnusedStartMigrationTimestamp{1, 1};

    class Invocation : public InvocationBase {

    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::IllegalOperation,
                    "tenant migrations are not available on config servers",
                    !serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer));

            uassert(ErrorCodes::IllegalOperation,
                    "tenant migrations are only available if --serverless is enabled",
                    repl::ReplicationCoordinator::get(opCtx)->getSettings().isServerless());

            uassert(ErrorCodes::IllegalOperation,
                    "Cannot run tenant migration with x509 authentication",
                    repl::tenantMigrationDisableX509Auth);

            const auto& cmd = request();
            const auto migrationProtocol = cmd.getProtocol().value_or(kDefaultMigrationProtocol);
            const auto& tenantId = cmd.getTenantId();
            const auto& tenantIds = cmd.getTenantIds();

            tenant_migration_util::protocolTenantIdCompatibilityCheck(migrationProtocol, tenantId);
            tenant_migration_util::protocolTenantIdsCompatibilityCheck(migrationProtocol,
                                                                       tenantIds);
            tenant_migration_util::protocolCheckRecipientForgetDecision(migrationProtocol,
                                                                        cmd.getDecision());

            if (MONGO_unlikely(returnResponseOkForRecipientForgetMigrationCmd.shouldFail())) {
                LOGV2(5949502,
                      "Immediately returning ok because failpoint is enabled",
                      "migrationId"_attr = cmd.getMigrationId(),
                      "fpName"_attr = returnResponseOkForRecipientForgetMigrationCmd.getName());
                return;
            }

            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            switch (migrationProtocol) {
                case MigrationProtocolEnum::kMultitenantMigrations:
                    return _handleMTMRecipientForgetMigrationCmd(opCtx, cmd);
                default:
                    MONGO_UNREACHABLE;
            }

            MONGO_UNREACHABLE;
        }

    private:
        void _handleMTMRecipientForgetMigrationCmd(OperationContext* opCtx, const Request& cmd) {
            TenantMigrationRecipientDocument stateDoc(cmd.getMigrationId(),
                                                      cmd.getDonorConnectionString().toString(),
                                                      cmd.getTenantId()->toString(),
                                                      kUnusedStartMigrationTimestamp,
                                                      cmd.getReadPreferenceSettings());

            stateDoc.setProtocol(MigrationProtocolEnum::kMultitenantMigrations);
            // Set the state to 'kDone' so that we don't create a recipient access blocker
            // unnecessarily if this recipientForgetMigration command is received before a
            // recipientSyncData command or after the state doc is garbage collected.
            stateDoc.setState(TenantMigrationRecipientStateEnum::kDone);

            auto recipientService =
                repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext())
                    ->lookupServiceByName(repl::TenantMigrationRecipientService::
                                              kTenantMigrationRecipientServiceName);
            auto recipientInstance = repl::TenantMigrationRecipientService::Instance::getOrCreate(
                opCtx, recipientService, stateDoc.toBSON(), false);

            // Instruct the instance run() function to mark this migration garbage collectable.
            recipientInstance->onReceiveRecipientForgetMigration(
                opCtx, TenantMigrationRecipientStateEnum::kDone);
            recipientInstance->getForgetMigrationDurableFuture().get(opCtx);
        }

        void doCheckAuthorization(OperationContext* opCtx) const final {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(
                            ResourcePattern::forClusterResource(request().getDbName().tenantId()),
                            ActionType::runTenantMigration));
        }

        bool supportsWriteConcern() const override {
            return false;
        }

        NamespaceString ns() const override {
            return NamespaceString(request().getDbName());
        }
    };

    std::string help() const override {
        return "Interrupts tenant migration data sync and marks that the recipient's durable state "
               "machine may be garbage collected.";
    }

    bool adminOnly() const override {
        return true;
    }

    BasicCommand::AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return BasicCommand::AllowedOnSecondary::kNever;
    }
};
MONGO_REGISTER_COMMAND(RecipientForgetMigrationCmd).forShard();

}  // namespace
}  // namespace mongo
