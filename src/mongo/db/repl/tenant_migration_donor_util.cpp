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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTenantMigration

#include "mongo/platform/basic.h"
#include "mongo/util/str.h"

#include "mongo/db/repl/tenant_migration_donor_util.h"

#include "mongo/db/commands/tenant_migration_recipient_cmds_gen.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/tenant_migration_access_blocker_by_prefix.h"
#include "mongo/db/repl/tenant_migration_state_machine_gen.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/fail_point.h"

namespace mongo {

namespace tenant_migration_donor {

namespace {

const char kThreadNamePrefix[] = "TenantMigrationWorker-";
const char kPoolName[] = "TenantMigrationWorkerThreadPool";
const char kNetName[] = "TenantMigrationWorkerNetwork";

/**
 * Updates the TenantMigrationAccessBlocker when the tenant migration transitions to the blocking
 * state.
 */
void onTransitionToBlocking(OperationContext* opCtx,
                            const TenantMigrationDonorDocument& donorStateDoc) {
    invariant(donorStateDoc.getState() == TenantMigrationDonorStateEnum::kBlocking);
    invariant(donorStateDoc.getBlockTimestamp());

    auto& mtabByPrefix = TenantMigrationAccessBlockerByPrefix::get(opCtx->getServiceContext());
    auto mtab =
        mtabByPrefix.getTenantMigrationAccessBlockerForDbPrefix(donorStateDoc.getDatabasePrefix());

    if (!opCtx->writesAreReplicated()) {
        // A primary must create the TenantMigrationAccessBlocker and call startBlockingWrites on it
        // before reserving the OpTime for the "start blocking" write, so only secondaries create
        // the TenantMigrationAccessBlocker and call startBlockingWrites on it in the op observer.
        invariant(!mtab);

        mtab = std::make_shared<TenantMigrationAccessBlocker>(
            opCtx->getServiceContext(),
            tenant_migration_donor::makeTenantMigrationExecutor(opCtx->getServiceContext()),
            donorStateDoc.getDatabasePrefix().toString());
        mtabByPrefix.add(donorStateDoc.getDatabasePrefix(), mtab);
        mtab->startBlockingWrites();
    }

    invariant(mtab);

    // Both primaries and secondaries call startBlockingReadsAfter in the op observer, since
    // startBlockingReadsAfter just needs to be called before the "start blocking" write's oplog
    // hole is filled.
    mtab->startBlockingReadsAfter(donorStateDoc.getBlockTimestamp().get());
}

/**
 * Transitions the TenantMigrationAccessBlocker to the committed state.
 */
void onTransitionToCommitted(OperationContext* opCtx,
                             const TenantMigrationDonorDocument& donorStateDoc) {
    invariant(donorStateDoc.getState() == TenantMigrationDonorStateEnum::kCommitted);
    invariant(donorStateDoc.getCommitOrAbortOpTime());

    auto& mtabByPrefix = TenantMigrationAccessBlockerByPrefix::get(opCtx->getServiceContext());
    auto mtab =
        mtabByPrefix.getTenantMigrationAccessBlockerForDbPrefix(donorStateDoc.getDatabasePrefix());
    invariant(mtab);
    mtab->commit(donorStateDoc.getCommitOrAbortOpTime().get());
}

/**
 * Transitions the TenantMigrationAccessBlocker to the aborted state.
 */
void onTransitionToAborted(OperationContext* opCtx,
                           const TenantMigrationDonorDocument& donorStateDoc) {
    invariant(donorStateDoc.getState() == TenantMigrationDonorStateEnum::kAborted);
    invariant(donorStateDoc.getCommitOrAbortOpTime());

    auto& mtabByPrefix = TenantMigrationAccessBlockerByPrefix::get(opCtx->getServiceContext());
    auto mtab =
        mtabByPrefix.getTenantMigrationAccessBlockerForDbPrefix(donorStateDoc.getDatabasePrefix());
    invariant(mtab);
    mtab->abort(donorStateDoc.getCommitOrAbortOpTime().get());
}

}  // namespace

std::unique_ptr<executor::TaskExecutor> makeTenantMigrationExecutor(
    ServiceContext* serviceContext) {
    ThreadPool::Options tpOptions;
    tpOptions.threadNamePrefix = kThreadNamePrefix;
    tpOptions.poolName = kPoolName;
    tpOptions.maxThreads = ThreadPool::Options::kUnlimited;

    return std::make_unique<executor::ThreadPoolTaskExecutor>(
        std::make_unique<ThreadPool>(tpOptions),
        executor::makeNetworkInterface(kNetName, nullptr, nullptr));
}

void onDonorStateDocUpdate(OperationContext* opCtx, const BSONObj& donorStateDocBson) {
    auto donorStateDoc = TenantMigrationDonorDocument::parse(IDLParserErrorContext("donorStateDoc"),
                                                             donorStateDocBson);
    if (donorStateDoc.getExpireAt()) {
        TenantMigrationAccessBlockerByPrefix::get(opCtx->getServiceContext())
            .remove(donorStateDoc.getDatabasePrefix());
    } else {
        switch (donorStateDoc.getState()) {
            case TenantMigrationDonorStateEnum::kDataSync:
                break;
            case TenantMigrationDonorStateEnum::kBlocking:
                onTransitionToBlocking(opCtx, donorStateDoc);
                break;
            case TenantMigrationDonorStateEnum::kCommitted:
                onTransitionToCommitted(opCtx, donorStateDoc);
                break;
            case TenantMigrationDonorStateEnum::kAborted:
                onTransitionToAborted(opCtx, donorStateDoc);
                break;
            default:
                MONGO_UNREACHABLE;
        }
    }
}

void checkIfCanReadOrBlock(OperationContext* opCtx, StringData dbName) {
    auto mtab = TenantMigrationAccessBlockerByPrefix::get(opCtx->getServiceContext())
                    .getTenantMigrationAccessBlockerForDbName(dbName);

    if (!mtab) {
        return;
    }

    auto readConcernArgs = repl::ReadConcernArgs::get(opCtx);
    auto targetTimestamp = [&]() -> boost::optional<Timestamp> {
        if (auto afterClusterTime = readConcernArgs.getArgsAfterClusterTime()) {
            return afterClusterTime->asTimestamp();
        }
        if (auto atClusterTime = readConcernArgs.getArgsAtClusterTime()) {
            return atClusterTime->asTimestamp();
        }
        if (readConcernArgs.getLevel() == repl::ReadConcernLevel::kSnapshotReadConcern) {
            return repl::StorageInterface::get(opCtx)->getPointInTimeReadTimestamp(opCtx);
        }
        return boost::none;
    }();

    if (targetTimestamp) {
        mtab->checkIfCanDoClusterTimeReadOrBlock(opCtx, targetTimestamp.get());
    }
}

void checkIfLinearizableReadWasAllowedOrThrow(OperationContext* opCtx, StringData dbName) {
    if (repl::ReadConcernArgs::get(opCtx).getLevel() ==
        repl::ReadConcernLevel::kLinearizableReadConcern) {
        if (auto mtab = TenantMigrationAccessBlockerByPrefix::get(opCtx->getServiceContext())
                            .getTenantMigrationAccessBlockerForDbName(dbName)) {
            mtab->checkIfLinearizableReadWasAllowedOrThrow(opCtx);
        }
    }
}

void onWriteToDatabase(OperationContext* opCtx, StringData dbName) {
    auto& mtabByPrefix = TenantMigrationAccessBlockerByPrefix::get(opCtx->getServiceContext());
    auto mtab = mtabByPrefix.getTenantMigrationAccessBlockerForDbName(dbName);

    if (mtab) {
        mtab->checkIfCanWriteOrThrow();
    }
}

}  // namespace tenant_migration_donor

}  // namespace mongo
