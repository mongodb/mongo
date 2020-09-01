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

#include "mongo/db/commands.h"
#include "mongo/db/commands/tenant_migration_donor_cmds_gen.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/tenant_migration_donor_service.h"
#include "mongo/db/repl/tenant_migration_donor_util.h"

namespace mongo {
namespace {

class DonorStartMigrationCmd : public TypedCommand<DonorStartMigrationCmd> {
public:
    using Request = DonorStartMigration;

    class Invocation : public InvocationBase {

    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::CommandNotSupported,
                    "donorStartMigration command not enabled",
                    repl::enableTenantMigrations);

            const RequestType& requestBody = request();

            // TODO (SERVER-50483): Make donorStartMigration command check that the donor's host
            // is not present in 'recipientConnectionString'
            const auto donorConnectionString =
                repl::ReplicationCoordinator::get(opCtx)->getConfig().getConnectionString();
            uassert(ErrorCodes::InvalidOptions,
                    "recipient must be different from donor",
                    donorConnectionString.toString() !=
                        requestBody.getRecipientConnectionString().toString());

            const auto donorStateDoc =
                TenantMigrationDonorDocument(requestBody.getMigrationId(),
                                             requestBody.getRecipientConnectionString().toString(),
                                             requestBody.getReadPreference(),
                                             requestBody.getDatabasePrefix().toString(),
                                             TenantMigrationDonorStateEnum::kDataSync)
                    .toBSON();

            auto donorService =
                repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext())
                    ->lookupServiceByName(TenantMigrationDonorService::kServiceName);
            auto donor =
                TenantMigrationDonorService::Instance::getOrCreate(donorService, donorStateDoc);
            uassertStatusOK(donor->checkIfOptionsConflict(donorStateDoc));

            donor->getCompletionFuture().get();
        }

        void doCheckAuthorization(OperationContext* opCtx) const {}

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

class DonorWaitForMigrationToCommitCmd : public TypedCommand<DonorWaitForMigrationToCommitCmd> {
public:
    using Request = DonorWaitForMigrationToCommit;

    class Invocation : public InvocationBase {

    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::CommandNotSupported,
                    "donorWaitForMigrationToCommit command not enabled",
                    repl::enableTenantMigrations);
        }

    private:
        void doCheckAuthorization(OperationContext* opCtx) const {}

        bool supportsWriteConcern() const override {
            return false;
        }
        NamespaceString ns() const {
            return NamespaceString(request().getDbName(), "");
        }
    };

    std::string help() const override {
        return "Wait for migration to be committed.";
    }
    bool adminOnly() const override {
        return true;
    }

    BasicCommand::AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return BasicCommand::AllowedOnSecondary::kNever;
    }

} donorWaitForMigrationToCommitCmd;

class DonorForgetMigrationCmd : public TypedCommand<DonorForgetMigrationCmd> {
public:
    using Request = DonorForgetMigration;

    class Invocation : public InvocationBase {

    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::CommandNotSupported,
                    "donorForgetMigration command not enabled",
                    repl::enableTenantMigrations);
        }

    private:
        void doCheckAuthorization(OperationContext* opCtx) const {}

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

}  // namespace
}  // namespace mongo
