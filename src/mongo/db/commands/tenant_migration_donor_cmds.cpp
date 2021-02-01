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
#include "mongo/db/repl/tenant_migration_committed_info.h"
#include "mongo/db/repl/tenant_migration_donor_service.h"

namespace mongo {
namespace {

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
            uassert(ErrorCodes::CommandNotSupported,
                    "donorStartMigration command not enabled",
                    repl::feature_flags::gTenantMigrations.isEnabled(
                        serverGlobalParams.featureCompatibility));

            const auto& cmd = request();

            const TenantMigrationDonorDocument stateDoc(
                cmd.getMigrationId(),
                cmd.getRecipientConnectionString().toString(),
                cmd.getReadPreference(),
                cmd.getTenantId().toString(),
                cmd.getDonorCertificateForRecipient(),
                cmd.getRecipientCertificateForDonor());
            const auto stateDocBson = stateDoc.toBSON();

            auto donorService =
                repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext())
                    ->lookupServiceByName(TenantMigrationDonorService::kServiceName);
            auto donor = TenantMigrationDonorService::Instance::getOrCreate(
                opCtx, donorService, stateDocBson);

            // If the conflict is discovered here, it implies that there is an existing instance
            // with the same migrationId but different options (e.g. tenantId or
            // recipientConnectionString or readPreference).
            uassertStatusOK(donor->checkIfOptionsConflict(stateDoc));

            auto durableState = [&] {
                try {
                    return donor->getDurableState(opCtx);
                } catch (ExceptionFor<ErrorCodes::ConflictingOperationInProgress>&) {
                    // The conflict is discovered while inserting the donor instance's state doc.
                    // This implies that there is no other instance with the same migrationId, but
                    // there is another instance with the same tenantId. Therefore, the instance
                    // above was created by this command, so remove it.
                    donorService->releaseInstance(stateDocBson["_id"].wrap());
                    throw;
                }
            }();

            auto response = Response(durableState.state);
            if (durableState.abortReason) {
                BSONObjBuilder bob;
                durableState.abortReason.get().serializeErrorToBSON(&bob);
                response.setAbortReason(bob.obj());
            }

            return response;
        }

        void doCheckAuthorization(OperationContext* opCtx) const {
            uassert(ErrorCodes::Unauthorized,
                    "Unauthorized",
                    AuthorizationSession::get(opCtx->getClient())
                        ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                           ActionType::runTenantMigration));
        }

    private:
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
            uassert(ErrorCodes::CommandNotSupported,
                    "donorForgetMigration command not enabled",
                    repl::feature_flags::gTenantMigrations.isEnabled(
                        serverGlobalParams.featureCompatibility));

            const auto& cmd = request();

            auto donorService =
                repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext())
                    ->lookupServiceByName(TenantMigrationDonorService::kServiceName);
            auto donor = TenantMigrationDonorService::Instance::lookup(
                opCtx, donorService, BSON("_id" << cmd.getMigrationId()));
            uassert(ErrorCodes::NoSuchTenantMigration,
                    str::stream() << "Could not find tenant migration with id "
                                  << cmd.getMigrationId(),
                    donor);

            auto durableState = donor.get()->getDurableState(opCtx);
            uassert(ErrorCodes::TenantMigrationInProgress,
                    str::stream() << "Could not forget migration with id " << cmd.getMigrationId()
                                  << " since no decision has been made yet",
                    durableState.state == TenantMigrationDonorStateEnum::kCommitted ||
                        durableState.state == TenantMigrationDonorStateEnum::kAborted);

            donor.get().get()->onReceiveDonorForgetMigration();
            donor.get().get()->getCompletionFuture().get(opCtx);
        }

    private:
        void doCheckAuthorization(OperationContext* opCtx) const {
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
            uassert(ErrorCodes::CommandNotSupported,
                    "donorAbortMigration command not enabled",
                    repl::feature_flags::gTenantMigrations.isEnabled(
                        serverGlobalParams.featureCompatibility));

            const RequestType& cmd = request();

            auto donorService =
                repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext())
                    ->lookupServiceByName(TenantMigrationDonorService::kServiceName);
            auto donorPtr = TenantMigrationDonorService::Instance::lookup(
                opCtx, donorService, BSON("_id" << cmd.getMigrationId()));

            // If there is NoSuchTenantMigration, perform a noop write and wait for it to be
            // majority committed to verify that any durable data read up to this point is majority
            // committed.
            if (!donorPtr) {
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

            const auto& donor = donorPtr.get().get();

            // Ensure that we only are able to run donorAbortMigration after the donor inserts a
            // majority committed state document for the migration.
            donor->getInitialDonorStateDurableFuture().get(opCtx);
            donor->onReceiveDonorAbortMigration();
            donor->getDecisionFuture().get(opCtx);

            auto durableState = donor->getDurableState(opCtx);

            uassert(TenantMigrationCommittedInfo(donor->getTenantId().toString(),
                                                 donor->getRecipientConnectionString().toString()),
                    "Tenant migration already committed",
                    durableState.state == TenantMigrationDonorStateEnum::kAborted);
        }

    private:
        void doCheckAuthorization(OperationContext* opCtx) const {
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
