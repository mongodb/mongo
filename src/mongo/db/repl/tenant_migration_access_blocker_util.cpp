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

#include "mongo/db/repl/tenant_migration_access_blocker_util.h"

#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands/tenant_migration_recipient_cmds_gen.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/tenant_migration_access_blocker_registry.h"
#include "mongo/db/repl/tenant_migration_state_machine_gen.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/transport/service_executor.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future_util.h"

namespace mongo {

// Failpoint that will cause recoverTenantMigrationAccessBlockers to return early.
MONGO_FAIL_POINT_DEFINE(skipRecoverTenantMigrationAccessBlockers);

namespace tenant_migration_access_blocker {

namespace {

constexpr char kThreadNamePrefix[] = "TenantMigrationWorker-";
constexpr char kPoolName[] = "TenantMigrationWorkerThreadPool";
constexpr char kNetName[] = "TenantMigrationWorkerNetwork";

const auto donorStateDocToDeleteDecoration = OperationContext::declareDecoration<BSONObj>();

}  // namespace

std::shared_ptr<TenantMigrationDonorAccessBlocker> getTenantMigrationDonorAccessBlocker(
    ServiceContext* const serviceContext, StringData tenantId) {
    return checked_pointer_cast<TenantMigrationDonorAccessBlocker>(
        TenantMigrationAccessBlockerRegistry::get(serviceContext)
            .getTenantMigrationAccessBlockerForTenantId(tenantId));
}

TenantMigrationDonorDocument parseDonorStateDocument(const BSONObj& doc) {
    auto donorStateDoc =
        TenantMigrationDonorDocument::parse(IDLParserErrorContext("donorStateDoc"), doc);

    if (donorStateDoc.getExpireAt()) {
        uassert(ErrorCodes::BadValue,
                "contains \"expireAt\" but the migration has not committed or aborted",
                donorStateDoc.getState() == TenantMigrationDonorStateEnum::kCommitted ||
                    donorStateDoc.getState() == TenantMigrationDonorStateEnum::kAborted);
    }

    const std::string errmsg = str::stream() << "invalid donor state doc " << doc;

    switch (donorStateDoc.getState()) {
        case TenantMigrationDonorStateEnum::kUninitialized:
            break;
        case TenantMigrationDonorStateEnum::kDataSync:
            uassert(ErrorCodes::BadValue,
                    errmsg,
                    !donorStateDoc.getBlockTimestamp() && !donorStateDoc.getCommitOrAbortOpTime() &&
                        !donorStateDoc.getAbortReason());
            break;
        case TenantMigrationDonorStateEnum::kBlocking:
            uassert(ErrorCodes::BadValue,
                    errmsg,
                    donorStateDoc.getBlockTimestamp() && !donorStateDoc.getCommitOrAbortOpTime() &&
                        !donorStateDoc.getAbortReason());
            break;
        case TenantMigrationDonorStateEnum::kCommitted:
            uassert(ErrorCodes::BadValue,
                    errmsg,
                    donorStateDoc.getBlockTimestamp() && donorStateDoc.getCommitOrAbortOpTime() &&
                        !donorStateDoc.getAbortReason());
            break;
        case TenantMigrationDonorStateEnum::kAborted:
            uassert(ErrorCodes::BadValue, errmsg, donorStateDoc.getAbortReason());
            break;
        default:
            MONGO_UNREACHABLE;
    }

    return donorStateDoc;
}

void checkIfCanReadOrBlock(OperationContext* opCtx, StringData dbName) {
    auto mtab = TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                    .getTenantMigrationAccessBlockerForDbName(dbName);

    if (!mtab) {
        return;
    }

    // Source to cancel the timeout if the operation completed in time.
    CancelationSource cancelTimeoutSource;
    std::vector<ExecutorFuture<void>> futures;

    auto executor = mtab->getAsyncBlockingOperationsExecutor();
    futures.emplace_back(mtab->getCanReadFuture(opCtx).semi().thenRunOn(executor));

    // Optimisation: if the future is already ready, we are done.
    if (futures[0].isReady()) {
        futures[0].get();  // Throw if error.
        return;
    }

    if (opCtx->hasDeadline()) {
        auto deadlineReachedFuture =
            executor->sleepUntil(opCtx->getDeadline(), cancelTimeoutSource.token());
        // The timeout condition is optional with index #1.
        futures.push_back(std::move(deadlineReachedFuture));
    }

    const auto& [status, idx] = whenAny(std::move(futures)).get();

    if (idx == 0) {
        // Read unblock condition finished first.
        cancelTimeoutSource.cancel();
        uassertStatusOK(status);
    } else if (idx == 1) {
        // Deadline finished first, throw error.
        uassertStatusOK(Status(opCtx->getTimeoutError(),
                               "Read timed out waiting for tenant migration blocker",
                               mtab->getDebugInfo()));
    }
}

void checkIfLinearizableReadWasAllowedOrThrow(OperationContext* opCtx, StringData dbName) {
    if (repl::ReadConcernArgs::get(opCtx).getLevel() ==
        repl::ReadConcernLevel::kLinearizableReadConcern) {
        if (auto mtab = TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                            .getTenantMigrationAccessBlockerForDbName(dbName)) {
            mtab->checkIfLinearizableReadWasAllowedOrThrow(opCtx);
        }
    }
}

void onWriteToDatabase(OperationContext* opCtx, StringData dbName) {
    auto mtab = TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                    .getTenantMigrationAccessBlockerForDbName(dbName);

    if (mtab) {
        mtab->checkIfCanWriteOrThrow();
    }
}

Status checkIfCanBuildIndex(OperationContext* opCtx, StringData dbName) {
    auto mtab = TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                    .getTenantMigrationAccessBlockerForDbName(dbName);

    if (mtab) {
        // This log is included for synchronization of the tenant migration buildindex jstests.
        auto status = mtab->checkIfCanBuildIndex();
        LOGV2_DEBUG(4886202,
                    1,
                    "Checked if tenant migration on database prevents index builds",
                    "db"_attr = dbName,
                    "error"_attr = status);
        return status;
    }
    return Status::OK();
}

void recoverTenantMigrationAccessBlockers(OperationContext* opCtx) {
    TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext()).shutDown();

    if (MONGO_unlikely(skipRecoverTenantMigrationAccessBlockers.shouldFail())) {
        return;
    }

    PersistentTaskStore<TenantMigrationDonorDocument> store(
        NamespaceString::kTenantMigrationDonorsNamespace);
    Query query;

    store.forEach(opCtx, query, [&](const TenantMigrationDonorDocument& doc) {
        // Skip creating a TenantMigrationDonorAccessBlocker for aborted migrations that have been
        // marked as garbage collected.
        if (doc.getExpireAt() && doc.getState() == TenantMigrationDonorStateEnum::kAborted)
            return true;

        auto mtab = std::make_shared<TenantMigrationDonorAccessBlocker>(
            opCtx->getServiceContext(),
            doc.getTenantId().toString(),
            doc.getRecipientConnectionString().toString());

        TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
            .add(doc.getTenantId(), mtab);

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
                mtab->setCommitOpTime(opCtx, doc.getCommitOrAbortOpTime().get());
                break;
            case TenantMigrationDonorStateEnum::kAborted:
                if (doc.getBlockTimestamp()) {
                    mtab->startBlockingWrites();
                    mtab->startBlockingReadsAfter(doc.getBlockTimestamp().get());
                }
                mtab->setAbortOpTime(opCtx, doc.getCommitOrAbortOpTime().get());
                break;
            default:
                MONGO_UNREACHABLE;
        }
        return true;
    });
}

void handleTenantMigrationConflict(OperationContext* opCtx, Status status) {
    auto migrationConflictInfo = status.extraInfo<TenantMigrationConflictInfo>();
    invariant(migrationConflictInfo);
    auto mtab = migrationConflictInfo->getTenantMigrationAccessBlocker();
    invariant(mtab);
    uassertStatusOK(
        mtab->waitUntilCommittedOrAborted(opCtx, migrationConflictInfo->getOperationType()));
}

void performNoopWrite(OperationContext* opCtx, StringData msg) {
    const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    AutoGetOplog oplogWrite(opCtx, OplogAccessMode::kWrite);
    uassert(ErrorCodes::NotWritablePrimary,
            "Not primary when performing noop write for {}"_format(msg),
            replCoord->canAcceptWritesForDatabase(opCtx, "admin"));

    writeConflictRetry(
        opCtx, "performNoopWrite", NamespaceString::kRsOplogNamespace.ns(), [&opCtx, &msg] {
            WriteUnitOfWork wuow(opCtx);
            opCtx->getClient()->getServiceContext()->getOpObserver()->onOpMessage(
                opCtx, BSON("msg" << msg));
            wuow.commit();
        });
}

void createRetryableWritesView(OperationContext* opCtx, Database* db) {
    writeConflictRetry(
        opCtx, "createDonorOplogView", "local.system.tenantMigration.oplogView", [&] {
            {
                // Create 'system.views' in a separate WUOW if it does not exist.
                WriteUnitOfWork wuow(opCtx);
                CollectionPtr coll = CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(
                    opCtx, NamespaceString(db->getSystemViewsName()));
                if (!coll) {
                    coll = db->createCollection(opCtx, NamespaceString(db->getSystemViewsName()));
                }
                invariant(coll);
                wuow.commit();
            }

            // First match entries with a `stmtId` so that we're filtering for retryable writes
            // oplog entries. Pass the result into the next stage of the pipeline and only project
            // the fields that a tenant migration recipient needs to refetch retryable writes oplog
            // entries: `ts`, `prevOpTime`, `preImageOpTime`, and `postImageOpTime`.
            CollectionOptions options;
            options.viewOn = NamespaceString::kRsOplogNamespace.coll().toString();
            options.pipeline = BSON_ARRAY(
                BSON("$match" << BSON("stmtId" << BSON("$exists" << true)))
                << BSON("$project" << BSON("_id"
                                           << "$ts"
                                           << "ns" << 1 << "ts" << 1 << "prevOpTime" << 1
                                           << "preImageOpTime" << 1 << "postImageOpTime" << 1)));

            WriteUnitOfWork wuow(opCtx);
            uassertStatusOK(db->createView(
                opCtx, NamespaceString("local.system.tenantMigration.oplogView"), options));
            wuow.commit();
        });
}

}  // namespace tenant_migration_access_blocker

}  // namespace mongo
