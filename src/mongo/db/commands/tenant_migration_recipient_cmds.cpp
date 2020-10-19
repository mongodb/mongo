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
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/db/commands.h"
#include "mongo/db/commands/tenant_migration_donor_cmds_gen.h"
#include "mongo/db/commands/tenant_migration_recipient_cmds_gen.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/repl/tenant_migration_recipient_service.h"
#include "mongo/logv2/log.h"

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(returnResponseOkForRecipientSyncDataCmd);

class RecipientSyncDataCmd : public TypedCommand<RecipientSyncDataCmd> {
public:
    using Request = RecipientSyncData;
    using Response = RecipientSyncDataResponse;

    class Invocation : public InvocationBase {

    public:
        using InvocationBase::InvocationBase;

        Response typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::CommandNotSupported,
                    "recipientSyncData command not enabled",
                    repl::enableTenantMigrations);

            const auto& cmd = request();

            TenantMigrationRecipientDocument stateDoc(cmd.getMigrationId(),
                                                      cmd.getDonorConnectionString().toString(),
                                                      cmd.getTenantId().toString(),
                                                      cmd.getReadPreference());


            if (MONGO_unlikely(returnResponseOkForRecipientSyncDataCmd.shouldFail())) {
                LOGV2(4879608,
                      "'returnResponseOkForRecipientSyncDataCmd' failpoint enabled.",
                      "tenantMigrationRecipientInstance"_attr = stateDoc.toBSON());
                return Response(repl::OpTime());
            }

            auto recipientService =
                repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext())
                    ->lookupServiceByName(repl::TenantMigrationRecipientService::
                                              kTenantMigrationRecipientServiceName);
            auto recipientInstance = repl::TenantMigrationRecipientService::Instance::getOrCreate(
                opCtx, recipientService, stateDoc.toBSON());
            uassertStatusOK(recipientInstance->checkIfOptionsConflict(stateDoc));

            auto returnAfterReachingTimestamp = cmd.getReturnAfterReachingTimestamp();
            if (!returnAfterReachingTimestamp) {
                return Response(recipientInstance->waitUntilMigrationReachesConsistentState(opCtx));
            }

            return Response(recipientInstance->waitUntilTimestampIsMajorityCommitted(
                opCtx, *returnAfterReachingTimestamp));
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
        return "Internal replica set command; instructs the recipient to sync data as part of a "
               "tenant migration.";
    }

    bool adminOnly() const override {
        return true;
    }

    BasicCommand::AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return BasicCommand::AllowedOnSecondary::kNever;
    }

} recipientSyncDataCmd;

class RecipientForgetMigrationCmd : public TypedCommand<RecipientForgetMigrationCmd> {
public:
    using Request = RecipientForgetMigration;

    class Invocation : public InvocationBase {

    public:
        using InvocationBase::InvocationBase;

        void typedRun(OperationContext* opCtx) {
            uassert(ErrorCodes::CommandNotSupported,
                    "recipientForgetMigration command not enabled",
                    repl::enableTenantMigrations);
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
        return "Interrupts tenant migration data sync and marks that the recipient's durable state "
               "machine may be garbage collected.";
    }

    bool adminOnly() const override {
        return true;
    }

    BasicCommand::AllowedOnSecondary secondaryAllowed(ServiceContext*) const override {
        return BasicCommand::AllowedOnSecondary::kNever;
    }

} recipientForgetMigrationCmd;

}  // namespace
}  // namespace mongo
