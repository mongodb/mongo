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
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/tenant_migration_access_blocker_registry.h"
#include "mongo/db/repl/tenant_migration_conflict_info.h"
#include "mongo/db/repl/tenant_migration_state_machine_gen.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/logv2/log.h"
#include "mongo/transport/service_executor.h"
#include "mongo/util/assert_util.h"
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

std::shared_ptr<TenantMigrationRecipientAccessBlocker> getTenantMigrationRecipientAccessBlocker(
    ServiceContext* const serviceContext, StringData tenantId) {
    return checked_pointer_cast<TenantMigrationRecipientAccessBlocker>(
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
        case TenantMigrationDonorStateEnum::kAbortingIndexBuilds:
            uassert(ErrorCodes::BadValue,
                    errmsg,
                    !donorStateDoc.getBlockTimestamp() && !donorStateDoc.getCommitOrAbortOpTime() &&
                        !donorStateDoc.getAbortReason() &&
                        !donorStateDoc.getStartMigrationDonorTimestamp());
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

SemiFuture<void> checkIfCanReadOrBlock(OperationContext* opCtx, StringData dbName) {
    auto mtab = TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                    .getTenantMigrationAccessBlockerForDbName(dbName);

    if (!mtab) {
        return Status::OK();
    }

    // Source to cancel the timeout if the operation completed in time.
    CancelationSource cancelTimeoutSource;

    auto canReadFuture = mtab->getCanReadFuture(opCtx);

    // Optimisation: if the future is already ready, we are done.
    if (canReadFuture.isReady()) {
        auto status = canReadFuture.getNoThrow();
        mtab->recordTenantMigrationError(status);
        return status;
    }

    auto executor = mtab->getAsyncBlockingOperationsExecutor();
    std::vector<ExecutorFuture<void>> futures;
    futures.emplace_back(std::move(canReadFuture).semi().thenRunOn(executor));

    if (opCtx->hasDeadline()) {
        auto deadlineReachedFuture =
            executor->sleepUntil(opCtx->getDeadline(), cancelTimeoutSource.token());
        // The timeout condition is optional with index #1.
        futures.push_back(std::move(deadlineReachedFuture));
    }

    return whenAny(std::move(futures))
        .thenRunOn(executor)
        .then([cancelTimeoutSource, opCtx, mtab, executor](WhenAnyResult<void> result) mutable {
            const auto& [status, idx] = result;
            if (idx == 0) {
                // Read unblock condition finished first.
                cancelTimeoutSource.cancel();
                mtab->recordTenantMigrationError(status);
                return status;
            } else if (idx == 1) {
                // Deadline finished first, throw error.
                return Status(opCtx->getTimeoutError(),
                              "Read timed out waiting for tenant migration blocker",
                              mtab->getDebugInfo());
            }
            MONGO_UNREACHABLE;
        })
        .onError([cancelTimeoutSource](Status status) mutable {
            cancelTimeoutSource.cancel();
            return status;
        })
        .semi();  // To require continuation in the user executor.
}

void checkIfLinearizableReadWasAllowedOrThrow(OperationContext* opCtx, StringData dbName) {
    if (repl::ReadConcernArgs::get(opCtx).getLevel() ==
        repl::ReadConcernLevel::kLinearizableReadConcern) {
        if (auto mtab = TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                            .getTenantMigrationAccessBlockerForDbName(dbName)) {
            auto status = mtab->checkIfLinearizableReadWasAllowed(opCtx);
            mtab->recordTenantMigrationError(status);
            uassertStatusOK(status);
        }
    }
}

void checkIfCanWriteOrThrow(OperationContext* opCtx, StringData dbName) {
    auto mtab = TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                    .getTenantMigrationAccessBlockerForDbName(dbName);

    if (mtab) {
        auto status = mtab->checkIfCanWrite();
        mtab->recordTenantMigrationError(status);
        uassertStatusOK(status);
    }
}

Status checkIfCanBuildIndex(OperationContext* opCtx, StringData dbName) {
    auto mtab = TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                    .getTenantMigrationAccessBlockerForDbName(dbName);

    if (mtab) {
        // This log is included for synchronization of the tenant migration buildindex jstests.
        auto status = mtab->checkIfCanBuildIndex();
        mtab->recordTenantMigrationError(status);
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

    // Recover TenantMigrationDonorAccessBlockers.
    PersistentTaskStore<TenantMigrationDonorDocument> donorStore(
        NamespaceString::kTenantMigrationDonorsNamespace);

    donorStore.forEach(opCtx, {}, [&](const TenantMigrationDonorDocument& doc) {
        // Skip creating a TenantMigrationDonorAccessBlocker for aborted migrations that have been
        // marked as garbage collected.
        if (doc.getExpireAt() && doc.getState() == TenantMigrationDonorStateEnum::kAborted) {
            return true;
        }

        auto mtab = std::make_shared<TenantMigrationDonorAccessBlocker>(
            opCtx->getServiceContext(),
            doc.getTenantId().toString(),
            doc.getRecipientConnectionString().toString());

        TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
            .add(doc.getTenantId(), mtab);

        switch (doc.getState()) {
            case TenantMigrationDonorStateEnum::kAbortingIndexBuilds:
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
            case TenantMigrationDonorStateEnum::kUninitialized:
                MONGO_UNREACHABLE;
        }
        return true;
    });

    // Recover TenantMigrationRecipientAccessBlockers.
    PersistentTaskStore<TenantMigrationRecipientDocument> recipientStore(
        NamespaceString::kTenantMigrationRecipientsNamespace);

    recipientStore.forEach(opCtx, {}, [&](const TenantMigrationRecipientDocument& doc) {
        // Skip creating a TenantMigrationRecipientAccessBlocker for aborted migrations that have
        // been marked as garbage collected.
        if (doc.getExpireAt() && doc.getState() == TenantMigrationRecipientStateEnum::kStarted) {
            return true;
        }

        if (!doc.getDataConsistentStopDonorOpTime()) {
            return true;
        }

        auto mtab = std::make_shared<TenantMigrationRecipientAccessBlocker>(
            opCtx->getServiceContext(),
            doc.getId(),
            doc.getTenantId().toString(),
            doc.getDonorConnectionString().toString());

        TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
            .add(doc.getTenantId(), mtab);

        switch (doc.getState()) {
            case TenantMigrationRecipientStateEnum::kDone:
            case TenantMigrationRecipientStateEnum::kStarted:
                break;
            case TenantMigrationRecipientStateEnum::kConsistent:
                if (doc.getRejectReadsBeforeTimestamp()) {
                    mtab->startRejectingReadsBefore(doc.getRejectReadsBeforeTimestamp().get());
                }
                break;
            case TenantMigrationRecipientStateEnum::kUninitialized:
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
    auto migrationStatus =
        mtab->waitUntilCommittedOrAborted(opCtx, migrationConflictInfo->getOperationType());
    mtab->recordTenantMigrationError(migrationStatus);
    uassertStatusOK(migrationStatus);
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

bool inRecoveryMode(OperationContext* opCtx) {
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (!replCoord->isReplEnabled()) {
        return false;
    }

    return replCoord->getMemberState().startup() || replCoord->getMemberState().startup2() ||
        replCoord->getMemberState().rollback();
}

}  // namespace tenant_migration_access_blocker

}  // namespace mongo
