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
#include "mongo/db/namespace_string.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/tenant_migration_access_blocker_by_prefix.h"
#include "mongo/db/repl/tenant_migration_state_machine_gen.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/fail_point.h"

namespace mongo {

// Failpoint that will cause recoverTenantMigrationAccessBlockers to return early.
MONGO_FAIL_POINT_DEFINE(skipRecoverTenantMigrationAccessBlockers);

namespace tenant_migration_donor {

namespace {

const char kThreadNamePrefix[] = "TenantMigrationWorker-";
const char kPoolName[] = "TenantMigrationWorkerThreadPool";
const char kNetName[] = "TenantMigrationWorkerNetwork";

const auto donorStateDocToDeleteDecoration = OperationContext::declareDecoration<BSONObj>();

/**
 * Updates the TenantMigrationAccessBlocker when the tenant migration transitions to the data sync
 * state.
 */
void onTransitionToDataSync(OperationContext* opCtx,
                            const TenantMigrationDonorDocument& donorStateDoc) {
    invariant(donorStateDoc.getState() == TenantMigrationDonorStateEnum::kDataSync);
    auto mtab = std::make_shared<TenantMigrationAccessBlocker>(
        opCtx->getServiceContext(),
        getTenantMigrationDonorExecutor(),
        donorStateDoc.getDatabasePrefix().toString());
    TenantMigrationAccessBlockerByPrefix::get(opCtx->getServiceContext())
        .add(donorStateDoc.getDatabasePrefix(), mtab);
}

/**
 * Updates the TenantMigrationAccessBlocker when the tenant migration transitions to the blocking
 * state.
 */
void onTransitionToBlocking(OperationContext* opCtx,
                            const TenantMigrationDonorDocument& donorStateDoc) {
    invariant(donorStateDoc.getState() == TenantMigrationDonorStateEnum::kBlocking);
    invariant(donorStateDoc.getBlockTimestamp());

    auto mtab = TenantMigrationAccessBlockerByPrefix::get(opCtx->getServiceContext())
                    .getTenantMigrationAccessBlockerForDbPrefix(donorStateDoc.getDatabasePrefix());
    invariant(mtab);

    if (!opCtx->writesAreReplicated()) {
        // A primary must call startBlockingWrites on the TenantMigrationAccessBlocker before
        // reserving the OpTime for the "start blocking" write, so only secondaries call
        // startBlockingWrites on the TenantMigrationAccessBlocker in the op observer.
        mtab->startBlockingWrites();
    }

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

    auto mtab = TenantMigrationAccessBlockerByPrefix::get(opCtx->getServiceContext())
                    .getTenantMigrationAccessBlockerForDbPrefix(donorStateDoc.getDatabasePrefix());
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

    auto mtab = TenantMigrationAccessBlockerByPrefix::get(opCtx->getServiceContext())
                    .getTenantMigrationAccessBlockerForDbPrefix(donorStateDoc.getDatabasePrefix());
    invariant(mtab);
    mtab->abort(donorStateDoc.getCommitOrAbortOpTime().get());
}

}  // namespace

std::shared_ptr<executor::TaskExecutor> getTenantMigrationDonorExecutor() {
    static Mutex mutex = MONGO_MAKE_LATCH("TenantMigrationDonorUtilExecutor::_mutex");
    static std::shared_ptr<executor::TaskExecutor> executor;

    stdx::lock_guard<Latch> lg(mutex);
    if (!executor) {
        ThreadPool::Options tpOptions;
        tpOptions.threadNamePrefix = kThreadNamePrefix;
        tpOptions.poolName = kPoolName;
        tpOptions.minThreads = 0;
        tpOptions.maxThreads = 16;

        executor = std::make_shared<executor::ThreadPoolTaskExecutor>(
            std::make_unique<ThreadPool>(tpOptions), executor::makeNetworkInterface(kNetName));
        executor->startup();
    }

    return executor;
}

void onInsertOrUpdate(OperationContext* opCtx, const BSONObj& donorStateDocBson) {
    // Disable the OpObserver during startup recovery, initial sync and rollback since the in-memory
    // state will be recovered by recoverTenantMigrationAccessBlockers.
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (replCoord->isReplEnabled()) {
        if (replCoord->getMemberState().startup() || replCoord->getMemberState().startup2() ||
            replCoord->getMemberState().rollback()) {
            return;
        }
    }
    auto donorStateDoc = TenantMigrationDonorDocument::parse(IDLParserErrorContext("donorStateDoc"),
                                                             donorStateDocBson);
    if (donorStateDoc.getExpireAt()) {
        return;
    }

    switch (donorStateDoc.getState()) {
        case TenantMigrationDonorStateEnum::kDataSync:
            onTransitionToDataSync(opCtx, donorStateDoc);
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

void onDelete(OperationContext* opCtx, const std::string dbPrefix) {
    TenantMigrationAccessBlockerByPrefix::get(opCtx->getServiceContext()).remove(dbPrefix);
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
    auto mtab = TenantMigrationAccessBlockerByPrefix::get(opCtx->getServiceContext())
                    .getTenantMigrationAccessBlockerForDbName(dbName);

    if (mtab) {
        mtab->checkIfCanWriteOrThrow();
    }
}

void recoverTenantMigrationAccessBlockers(OperationContext* opCtx) {
    if (MONGO_unlikely(skipRecoverTenantMigrationAccessBlockers.shouldFail())) {
        return;
    }

    PersistentTaskStore<TenantMigrationDonorDocument> store(
        NamespaceString::kTenantMigrationDonorsNamespace);
    Query query;

    store.forEach(opCtx, query, [&](const TenantMigrationDonorDocument& doc) {
        auto mtab =
            std::make_shared<TenantMigrationAccessBlocker>(opCtx->getServiceContext(),
                                                           getTenantMigrationDonorExecutor(),
                                                           doc.getDatabasePrefix().toString());

        TenantMigrationAccessBlockerByPrefix::get(opCtx->getServiceContext())
            .add(doc.getDatabasePrefix(), mtab);

        switch (doc.getState()) {
            case TenantMigrationDonorStateEnum::kDataSync:
                break;
            case TenantMigrationDonorStateEnum::kBlocking:
                invariant(doc.getBlockTimestamp());
                mtab->startBlockingWrites();
                mtab->startBlockingReadsAfter(doc.getBlockTimestamp().get());
                break;
            case TenantMigrationDonorStateEnum::kCommitted:
                invariant(doc.getBlockTimestamp());
                mtab->startBlockingWrites();
                mtab->startBlockingReadsAfter(doc.getBlockTimestamp().get());
                mtab->commit(doc.getCommitOrAbortOpTime().get());
                break;
            case TenantMigrationDonorStateEnum::kAborted:
                if (doc.getBlockTimestamp()) {
                    mtab->startBlockingWrites();
                    mtab->startBlockingReadsAfter(doc.getBlockTimestamp().get());
                }
                mtab->abort(doc.getCommitOrAbortOpTime().get());
                break;
            default:
                MONGO_UNREACHABLE;
        }
        return true;
    });
}

}  // namespace tenant_migration_donor

}  // namespace mongo
