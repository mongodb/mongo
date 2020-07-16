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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication;

#include "mongo/platform/basic.h"
#include "mongo/util/str.h"

#include "mongo/db/repl/migrating_tenant_donor_util.h"

#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/repl/migrate_tenant_state_machine_gen.h"
#include "mongo/db/repl/migrating_tenant_access_blocker_by_prefix.h"
#include "mongo/db/s/persistent_task_store.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/util/concurrency/thread_pool.h"


namespace mongo {

namespace migrating_tenant_donor_util {

namespace {

const char kThreadNamePrefix[] = "TenantMigrationWorker-";
const char kPoolName[] = "TenantMigrationWorkerThreadPool";
const char kNetName[] = "TenantMigrationWorkerNetwork";

/**
 * Updates the MigratingTenantAccessBlocker when the tenant migration transitions to the blocking
 * state.
 */
void onTransitionToBlocking(OperationContext* opCtx, TenantMigrationDonorDocument& donorDoc) {
    LOGV2(4917300, "Reached the beginning of the onTransitionToBlocking");
    invariant(donorDoc.getState() == TenantMigrationDonorStateEnum::kBlocking);
    invariant(donorDoc.getBlockTimestamp());
    LOGV2(4917300, "Passed invariants of the onTransitionToBlocking");

    auto& mtabByPrefix = MigratingTenantAccessBlockerByPrefix::get(opCtx->getServiceContext());
    auto mtab = mtabByPrefix.getMigratingTenantBlocker(donorDoc.getDatabasePrefix());

    if (!opCtx->writesAreReplicated()) {
        // A primary must create the MigratingTenantAccessBlocker and call startBlockingWrites on it
        // before reserving the OpTime for the "start blocking" write, so only secondaries create
        // the MigratingTenantAccessBlocker and call startBlockingWrites on it in the op observer.
        invariant(!mtab);

        mtab = std::make_shared<MigratingTenantAccessBlocker>(
            opCtx->getServiceContext(),
            migrating_tenant_donor_util::getTenantMigrationExecutor(opCtx->getServiceContext())
                .get());
        mtabByPrefix.add(donorDoc.getDatabasePrefix(), mtab);
        mtab->startBlockingWrites();
    }

    invariant(mtab);

    // Both primaries and secondaries call startBlockingReadsAfter in the op observer, since
    // startBlockingReadsAfter just needs to be called before the "start blocking" write's oplog
    // hole is filled.
    mtab->startBlockingReadsAfter(donorDoc.getBlockTimestamp().get());
}

/**
 * Creates a MigratingTenantAccess blocker, and makes it start blocking writes. Then adds it to
 * the MigratingTenantAccessBlockerByPrefix container using the donor document's databasePrefix as
 * the key.
 */
void startTenantMigrationBlockOnPrimary(OperationContext* opCtx,
                                        const TenantMigrationDonorDocument& donorDoc) {
    invariant(donorDoc.getState() == TenantMigrationDonorStateEnum::kDataSync);
    auto serviceContext = opCtx->getServiceContext();

    executor::TaskExecutor* mtabExecutor = getTenantMigrationExecutor(serviceContext).get();
    auto mtab = std::make_shared<MigratingTenantAccessBlocker>(serviceContext, mtabExecutor);

    mtab->startBlockingWrites();

    auto& mtabByPrefix = MigratingTenantAccessBlockerByPrefix::get(serviceContext);
    mtabByPrefix.add(donorDoc.getDatabasePrefix(), mtab);
}

/**
 *  Returns an updated donor state document that will be the same as the original except with the
 *  state set to blocking and the blockTimestamp set to the reserved Optime.
 */
TenantMigrationDonorDocument createUpdatedDonorStateDocument(
    const TenantMigrationDonorDocument& originalDoc, const OplogSlot& oplogSlot) {
    TenantMigrationDonorDocument updatedDoc = originalDoc;
    updatedDoc.setState(TenantMigrationDonorStateEnum::kBlocking);
    updatedDoc.setBlockTimestamp(oplogSlot.getTimestamp());
    return updatedDoc;
}

/**
 * Returns the arguments object for the update command with the _id as the criteria and the reserved
 * opTime as the oplogSlot.
 */
CollectionUpdateArgs createUpdateArgumentsForDonorStateDocument(
    const TenantMigrationDonorDocument& originalDoc,
    const OplogSlot& oplogSlot,
    const BSONObj& parsedUpdatedDoc) {
    CollectionUpdateArgs args;
    args.criteria = BSON("_id" << originalDoc.getId());
    args.oplogSlot = oplogSlot;
    args.update = parsedUpdatedDoc;

    return args;
}

/**
 * Returns the collection that stores the state machine documents for the donor.
 */
Collection* getTenantMigrationDonorsCollection(OperationContext* opCtx) {
    AutoGetCollection autoCollection(opCtx, NamespaceString::kMigrationDonorsNamespace, MODE_IX);
    return autoCollection.getCollection();
}

/**
 * After reserving the opTime for the write and creating the new updated document with the necessary
 * update arguments. It will send the update command to the tenant migration donors collection.
 */
void updateDonorStateDocument(OperationContext* opCtx,
                              Collection* collection,
                              const TenantMigrationDonorDocument& originalDoc) {

    // Reserve an opTime for the write and use it as the blockTimestamp for the migration.
    const auto originalRecordId =
        Helpers::findOne(opCtx, collection, originalDoc.toBSON(), false /* requireIndex */);
    invariant(!originalRecordId.isNull());

    auto oplogSlot = repl::LocalOplogInfo::get(opCtx)->getNextOpTimes(opCtx, 1U)[0];

    BSONObj parsedUpdatedDoc = createUpdatedDonorStateDocument(originalDoc, oplogSlot).toBSON();

    CollectionUpdateArgs args =
        createUpdateArgumentsForDonorStateDocument(originalDoc, oplogSlot, parsedUpdatedDoc);

    collection->updateDocument(
        opCtx,
        originalRecordId,
        Snapshotted<BSONObj>(opCtx->recoveryUnit()->getSnapshotId(), originalDoc.toBSON()),
        parsedUpdatedDoc,
        false,
        nullptr /* OpDebug* */,
        &args);
}
}  // namespace

/**
 *   TODO - Implement recipientSyncData command
 */
void dataSync(OperationContext* opCtx, const TenantMigrationDonorDocument& originalDoc) {
    // Send recipientSyncData.

    startTenantMigrationBlockOnPrimary(opCtx, originalDoc);

    // Update the on-disk state of the migration to "blocking" state.
    invariant(originalDoc.getState() == TenantMigrationDonorStateEnum::kDataSync);

    uassertStatusOK(writeConflictRetry(
        opCtx,
        "doStartBlockingWrite",
        NamespaceString::kMigrationDonorsNamespace.ns(),
        [&]() -> Status {
            AutoGetCollection autoCollection(
                opCtx, NamespaceString::kMigrationDonorsNamespace, MODE_IX);
            Collection* collection = autoCollection.getCollection();

            if (!collection) {
                return Status(ErrorCodes::NamespaceNotFound,
                              str::stream() << NamespaceString::kMigrationDonorsNamespace.ns()
                                            << " does not exist");
            }

            WriteUnitOfWork wuow(opCtx);
            updateDonorStateDocument(opCtx, collection, originalDoc);
            wuow.commit();

            return Status::OK();
        }));
}


std::shared_ptr<executor::TaskExecutor> getTenantMigrationExecutor(ServiceContext* serviceContext) {
    ThreadPool::Options tpOptions;
    tpOptions.threadNamePrefix = kThreadNamePrefix;
    tpOptions.poolName = kPoolName;
    tpOptions.maxThreads = ThreadPool::Options::kUnlimited;
    tpOptions.onCreateThread = [](const std::string& threadName) {
        Client::initThread(threadName.c_str());
    };

    return std::make_unique<executor::ThreadPoolTaskExecutor>(
        std::make_unique<ThreadPool>(tpOptions),
        executor::makeNetworkInterface(kNetName, nullptr, nullptr));
}

void onTenantMigrationDonorStateTransition(OperationContext* opCtx, const BSONObj& doc) {
    auto donorDoc = TenantMigrationDonorDocument::parse(IDLParserErrorContext("donorDoc"), doc);

    switch (donorDoc.getState()) {
        case TenantMigrationDonorStateEnum::kDataSync:
            break;
        case TenantMigrationDonorStateEnum::kBlocking:
            onTransitionToBlocking(opCtx, donorDoc);
            break;
        case TenantMigrationDonorStateEnum::kCommitted:
            break;
        case TenantMigrationDonorStateEnum::kAborted:
            break;
        default:
            MONGO_UNREACHABLE;
    }
}

/**
 * The function will persist(insert) the provided donorStateDocument into the config data on the
 * collection for the tenantMigration donors In order to maintain a stable state for the tenant
 * migration in case of node failure or restart.
 */
void persistDonorStateDocument(OperationContext* opCtx,
                               const TenantMigrationDonorDocument& donorStateDocument) {
    PersistentTaskStore<TenantMigrationDonorDocument> store(
        NamespaceString::kMigrationDonorsNamespace);
    try {
        store.add(opCtx, donorStateDocument);
    } catch (const ExceptionFor<ErrorCodes::DuplicateKey>&) {
        uasserted(
            4917300,
            str::stream()
                << "While attempting to persist the donor state machine for tenant migration"
                << ", found another document with the same migration id. Attempted migration: "
                << donorStateDocument.toBSON());
    }
}

}  // namespace migrating_tenant_donor_util

}  // namespace mongo
