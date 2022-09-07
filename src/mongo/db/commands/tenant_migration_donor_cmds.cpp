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

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/tenant_migration_donor_cmds_gen.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/tenant_migration_access_blocker_util.h"
#include "mongo/db/repl/tenant_migration_donor_service.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(returnResponseCommittedForDonorStartMigrationCmd);

class DonorStartMigrationCmd : public TypedCommand<DonorStartMigrationCmd> {
public:
    using Request = DonorStartMigration;
    using Response = DonorStartMigrationResponse;

    std::set<StringData> sensitiveFieldNames() const final {
        return {Request::kDonorCertificateForRecipientFieldName,
                Request::kRecipientCertificateForDonorFieldName};
    }

    class Invocation : public InvocationBase {

    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::IllegalOperation,
                    "tenant migrations are not available on config servers",
                    serverGlobalParams.clusterRole == ClusterRole::None ||
                        serverGlobalParams.clusterRole == ClusterRole::ShardServer);

            // (Generic FCV reference): This FCV reference should exist across LTS binary versions.
            uassert(
                5356100,
                "donorStartMigration not available while upgrading or downgrading the donor FCV",
                !serverGlobalParams.featureCompatibility.isUpgradingOrDowngrading());

            const auto& cmd = request();
            const auto migrationProtocol = cmd.getProtocol().value_or(kDefaultMigrationProtocol);
            const auto& tenantId = cmd.getTenantId();

            tenant_migration_util::protocolTenantIdCompatibilityCheck(migrationProtocol, tenantId);
            tenant_migration_util::protocolStorageOptionsCompatibilityCheck(opCtx,
                                                                            migrationProtocol);
            tenant_migration_util::protocolReadPreferenceCompatibilityCheck(
                opCtx, migrationProtocol, cmd.getReadPreference());

            // tenantId will be set to empty string for the "shard merge" protocol.
            TenantMigrationDonorDocument stateDoc(cmd.getMigrationId(),
                                                  cmd.getRecipientConnectionString().toString(),
                                                  cmd.getReadPreference(),
                                                  tenantId.value_or("").toString());

            if (!repl::tenantMigrationDisableX509Auth) {
                uassert(ErrorCodes::InvalidOptions,
                        str::stream() << "'" << Request::kDonorCertificateForRecipientFieldName
                                      << "' is a required field",
                        cmd.getDonorCertificateForRecipient());
                uassert(ErrorCodes::InvalidOptions,
                        str::stream() << "'" << Request::kRecipientCertificateForDonorFieldName
                                      << "' is a required field",
                        cmd.getRecipientCertificateForDonor());
                stateDoc.setDonorCertificateForRecipient(cmd.getDonorCertificateForRecipient());
                stateDoc.setRecipientCertificateForDonor(cmd.getRecipientCertificateForDonor());
            }

            stateDoc.setProtocol(migrationProtocol);

            const auto stateDocBson = stateDoc.toBSON();

            if (MONGO_unlikely(returnResponseCommittedForDonorStartMigrationCmd.shouldFail())) {
                LOGV2(5949401,
                      "Immediately returning committed because "
                      "'returnResponseCommittedForDonorStartMigrationCmd' failpoint is enabled",
                      "tenantMigrationDonorInstance"_attr = stateDoc.toBSON());
                return Response(TenantMigrationDonorStateEnum::kCommitted);
            }

            auto donorService =
                repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext())
                    ->lookupServiceByName(TenantMigrationDonorService::kServiceName);
            auto donor = TenantMigrationDonorService::Instance::getOrCreate(
                opCtx, donorService, stateDocBson);

            // always ensure we wait for the initial state document to be inserted.
            donor->getInitialStateDocumentDurableFuture().get(opCtx);
            auto durableState = donor->getDurableState();

            auto response = Response(durableState->state);
            if (durableState->abortReason) {
                BSONObjBuilder bob;
                durableState->abortReason->serializeErrorToBSON(&bob);
                response.setAbortReason(bob.obj());
            }

            return response;
        }

    private:
        void doCheckAuthorization(OperationContext* opCtx) const final {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::runTenantMigration));
        }

        bool supportsWriteConcern() const override {
            return false;
        }
        NamespaceString ns() const {
            return NamespaceString(request().getDbName(), "");
        }
    };

    std::string help() const {
        return "Start migrating databases whose names match the specified prefix to the specified "
               "replica set.";
    }
    bool adminOnly() const override {
        return true;
    }

    BasicCommand::AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return BasicCommand::AllowedOnSecondary::kNever;
    }

} donorStartMigrationCmd;

class DonorForgetMigrationCmd : public TypedCommand<DonorForgetMigrationCmd> {
public:
    using Request = DonorForgetMigration;

    class Invocation : public InvocationBase {

    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::IllegalOperation,
                    "tenant migrations are not available on config servers",
                    serverGlobalParams.clusterRole == ClusterRole::None ||
                        serverGlobalParams.clusterRole == ClusterRole::ShardServer);

            const auto& cmd = request();

            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();
            auto donorService =
                repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext())
                    ->lookupServiceByName(TenantMigrationDonorService::kServiceName);
            auto optionalDonor = TenantMigrationDonorService::Instance::lookup(
                opCtx, donorService, BSON("_id" << cmd.getMigrationId()));
            uassert(ErrorCodes::NoSuchTenantMigration,
                    str::stream() << "Could not find tenant migration with id "
                                  << cmd.getMigrationId(),
                    optionalDonor);

            // Retrieve the shared_ptr from boost::optional to improve readability
            auto donorPtr = optionalDonor.value();

            // always ensure we wait for the initial state document to be inserted.
            donorPtr->getInitialStateDocumentDurableFuture().get(opCtx);

            auto durableState = donorPtr->getDurableState();
            uassert(ErrorCodes::TenantMigrationInProgress,
                    str::stream() << "Could not forget migration with id " << cmd.getMigrationId()
                                  << " since no decision has been made yet",
                    durableState->state == TenantMigrationDonorStateEnum::kCommitted ||
                        durableState->state == TenantMigrationDonorStateEnum::kAborted);

            donorPtr->onReceiveDonorForgetMigration();
            donorPtr->getForgetMigrationDurableFuture().get(opCtx);
        }

    private:
        void doCheckAuthorization(OperationContext* opCtx) const final {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::runTenantMigration));
        }

        bool supportsWriteConcern() const override {
            return false;
        }
        NamespaceString ns() const {
            return NamespaceString(request().getDbName(), "");
        }
    };

    std::string help() const override {
        return "Forget a migration";
    }

    bool adminOnly() const override {
        return true;
    }

    BasicCommand::AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return BasicCommand::AllowedOnSecondary::kNever;
    }
} donorForgetMigrationCmd;

class DonorAbortMigrationCmd : public TypedCommand<DonorAbortMigrationCmd> {
public:
    using Request = DonorAbortMigration;

    class Invocation : public InvocationBase {

    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::IllegalOperation,
                    "tenant migrations are not available on config servers",
                    serverGlobalParams.clusterRole == ClusterRole::None ||
                        serverGlobalParams.clusterRole == ClusterRole::ShardServer);

            const RequestType& cmd = request();

            auto donorService =
                repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext())
                    ->lookupServiceByName(TenantMigrationDonorService::kServiceName);
            auto optionalDonor = TenantMigrationDonorService::Instance::lookup(
                opCtx, donorService, BSON("_id" << cmd.getMigrationId()));

            // If there is NoSuchTenantMigration, perform a noop write and wait for it to be
            // majority committed to verify that any durable data read up to this point is majority
            // committed.
            if (!optionalDonor) {
                tenant_migration_access_blocker::performNoopWrite(opCtx,
                                                                  "NoSuchTenantMigration error");

                auto& replClient = repl::ReplClientInfo::forClient(opCtx->getClient());
                WriteConcernResult writeConcernResult;
                WriteConcernOptions majority(WriteConcernOptions::kMajority,
                                             WriteConcernOptions::SyncMode::UNSET,
                                             WriteConcernOptions::kWriteConcernTimeoutUserCommand);
                uassertStatusOK(waitForWriteConcern(
                    opCtx, replClient.getLastOp(), majority, &writeConcernResult));

                uasserted(ErrorCodes::NoSuchTenantMigration,
                          str::stream() << "Could not find tenant migration with id "
                                        << cmd.getMigrationId());
            }

            // Retrieve the shared_ptr from boost::optional to improve readability
            auto donorPtr = optionalDonor.value();

            donorPtr->onReceiveDonorAbortMigration();
            donorPtr->getDecisionFuture().get(opCtx);
            auto durableState = donorPtr->getDurableState();

            uassert(ErrorCodes::TenantMigrationCommitted,
                    "Tenant migration already committed",
                    durableState->state == TenantMigrationDonorStateEnum::kAborted);
        }

    private:
        void doCheckAuthorization(OperationContext* opCtx) const final {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::runTenantMigration));
        }

        bool supportsWriteConcern() const override {
            return false;
        }
        NamespaceString ns() const {
            return NamespaceString(request().getDbName(), "");
        }
    };

    std::string help() const override {
        return "Abort a migration";
    }

    bool adminOnly() const override {
        return true;
    }

    BasicCommand::AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return BasicCommand::AllowedOnSecondary::kNever;
    }
} donorAbortMigrationCmd;

}  // namespace
}  // namespace mongo
