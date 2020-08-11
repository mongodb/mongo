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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/tenant_migration_recipient_entry_helpers.h"
#include "mongo/db/repl/tenant_migration_recipient_service.h"
#include "mongo/db/repl/tenant_migration_state_machine_gen.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"

namespace mongo {
namespace repl {

// Fails before waiting for the state doc to be majority replicated.
MONGO_FAIL_POINT_DEFINE(failWhilePersistingTenantMigrationRecipientInstanceStateDoc);

TenantMigrationRecipientService::TenantMigrationRecipientService(ServiceContext* serviceContext)
    : PrimaryOnlyService(serviceContext) {}

StringData TenantMigrationRecipientService::getServiceName() const {
    return kTenantMigrationRecipientServiceName;
}

NamespaceString TenantMigrationRecipientService::getStateDocumentsNS() const {
    return NamespaceString::kTenantMigrationRecipientsNamespace;
}

ThreadPool::Limits TenantMigrationRecipientService::getThreadPoolLimits() const {
    // TODO SERVER-50669: This will be replaced by a tunable server parameter.
    return ThreadPool::Limits();
}

std::shared_ptr<PrimaryOnlyService::Instance> TenantMigrationRecipientService::constructInstance(
    BSONObj initialStateDoc) const {
    return std::make_shared<TenantMigrationRecipientService::Instance>(initialStateDoc);
}

TenantMigrationRecipientService::Instance::Instance(BSONObj stateDoc)
    : PrimaryOnlyService::TypedInstance<Instance>() {
    _stateDoc = TenantMigrationRecipientDocument::parse(IDLParserErrorContext("recipientStateDoc"),
                                                        stateDoc);
}

SemiFuture<void> TenantMigrationRecipientService::Instance::run(
    std::shared_ptr<executor::ScopedTaskExecutor> executor) noexcept {
    return ExecutorFuture(**executor)
        .then([this]() -> SharedSemiFuture<void> {
            auto uniqueOpCtx = cc().makeOperationContext();
            auto opCtx = uniqueOpCtx.get();

            // The instance is marked as garbage collect if the migration is either
            // committed or aborted on donor side. So, don't start the recipient task if the
            // instance state doc is marked for garbage collect.
            uassert(ErrorCodes::IllegalOperation,
                    str::stream() << "Can't start the data sync as the state doc is already marked "
                                     "for garbage collect for migration uuid: "
                                  << getMigrationUUID(),
                    !isMarkedForGarbageCollect());

            auto lastOpBeforeRun = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();

            // Persist the state doc before starting the data sync.
            auto status = tenantMigrationRecipientEntryHelpers::insertStateDoc(opCtx, _stateDoc);

            // TODO SERVER-50742: Ignoring duplicate check step should be removed.
            // We can hit duplicate key error when the instances are rebuilt on a new primary after
            // step up. So, it's ok to ignore duplicate key errors.
            if (status != ErrorCodes::DuplicateKey) {
                uassertStatusOK(status);
            }

            auto lastOpAfterRun = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
            // No writes happened implies that the state doc is already on disk. This can happen
            // only when the instances are rebuilt on node step up. And,
            // PrimaryOnlyService::onStepUp() waits for majority commit of the primary no-op oplog
            // entry written by the node in the newer term before scheduling the Instance::run().
            // So, it's safe to assume that instance's state document written in an older term on
            // disk won't get rolled back for step up case.
            if (lastOpBeforeRun == lastOpAfterRun) {
                // TODO SERVER-50742: Add an invariant check to make sure this case can happen only
                // for step up.
                return {Future<void>::makeReady()};
            }

            if (MONGO_unlikely(
                    failWhilePersistingTenantMigrationRecipientInstanceStateDoc.shouldFail())) {
                LOGV2(4878500, "Persisting state doc failed due to fail point enabled.");
                uassert(ErrorCodes::NotWritablePrimary, "not writable primary ", false);
            }

            // Wait for the state doc to be majority replicated.
            return WaitForMajorityService::get(opCtx->getServiceContext())
                .waitUntilMajority(lastOpAfterRun);
        })
        .then([this] {
            // TODO SERVER-48808: Run cloners in MigrationServiceInstance
            // TODO SERVER-48811: Oplog fetching in MigrationServiceInstance
        })
        .onCompletion([this](Status status) {
            LOGV2(4878501,
                  "Tenant Recipient data sync completed.",
                  "migrationId"_attr = getMigrationUUID(),
                  "dbPrefix"_attr = _stateDoc.getDatabasePrefix(),
                  "status"_attr = status);
            return status;
        })
        .semi();
}

const UUID& TenantMigrationRecipientService::Instance::getMigrationUUID() const {
    stdx::lock_guard lk(_mutex);
    return _stateDoc.getId();
}

bool TenantMigrationRecipientService::Instance::isMarkedForGarbageCollect() const {
    stdx::lock_guard lk(_mutex);
    return _stateDoc.getGarbageCollect();
}

}  // namespace repl
}  // namespace mongo
