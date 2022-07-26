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


#include "mongo/platform/basic.h"
#include "mongo/util/str.h"

#include "mongo/db/repl/tenant_migration_access_blocker_util.h"

#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/tenant_migration_access_blocker_registry.h"
#include "mongo/db/repl/tenant_migration_conflict_info.h"
#include "mongo/db/repl/tenant_migration_state_machine_gen.h"
#include "mongo/db/serverless/shard_split_state_machine_gen.h"
#include "mongo/db/serverless/shard_split_utils.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/logv2/log.h"
#include "mongo/transport/service_executor.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTenantMigration


namespace mongo {

// Failpoint that will cause recoverTenantMigrationAccessBlockers to return early.
MONGO_FAIL_POINT_DEFINE(skipRecoverTenantMigrationAccessBlockers);

// Signals that we have checked that we can build an index.
MONGO_FAIL_POINT_DEFINE(haveCheckedIfIndexBuildableDuringTenantMigration);

namespace tenant_migration_access_blocker {

namespace {
using MtabType = TenantMigrationAccessBlocker::BlockerType;

constexpr char kThreadNamePrefix[] = "TenantMigrationWorker-";
constexpr char kPoolName[] = "TenantMigrationWorkerThreadPool";
constexpr char kNetName[] = "TenantMigrationWorkerNetwork";

const auto donorStateDocToDeleteDecoration = OperationContext::declareDecoration<BSONObj>();
}  // namespace

std::shared_ptr<TenantMigrationDonorAccessBlocker> getTenantMigrationDonorAccessBlocker(
    ServiceContext* const serviceContext, StringData tenantId) {
    return checked_pointer_cast<TenantMigrationDonorAccessBlocker>(
        TenantMigrationAccessBlockerRegistry::get(serviceContext)
            .getTenantMigrationAccessBlockerForTenantId(tenantId, MtabType::kDonor));
}

std::shared_ptr<TenantMigrationRecipientAccessBlocker> getTenantMigrationRecipientAccessBlocker(
    ServiceContext* const serviceContext, StringData tenantId) {
    return checked_pointer_cast<TenantMigrationRecipientAccessBlocker>(
        TenantMigrationAccessBlockerRegistry::get(serviceContext)
            .getTenantMigrationAccessBlockerForTenantId(tenantId, MtabType::kRecipient));
}

void startRejectingReadsBefore(OperationContext* opCtx, UUID migrationId, mongo::Timestamp ts) {
    auto callback = [&](std::shared_ptr<TenantMigrationAccessBlocker> mtab) {
        auto recipientMtab = checked_pointer_cast<TenantMigrationRecipientAccessBlocker>(mtab);
        recipientMtab->startRejectingReadsBefore(ts);
    };

    TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
        .applyAll(TenantMigrationAccessBlocker::BlockerType::kRecipient, callback);
}

void addTenantMigrationRecipientAccessBlocker(ServiceContext* serviceContext,
                                              StringData tenantId,
                                              UUID migrationId,
                                              MigrationProtocolEnum protocol,
                                              StringData donorConnectionString) {
    if (getTenantMigrationRecipientAccessBlocker(serviceContext, tenantId)) {
        return;
    }

    auto mtab =
        std::make_shared<TenantMigrationRecipientAccessBlocker>(serviceContext,
                                                                migrationId,
                                                                tenantId.toString(),
                                                                protocol,
                                                                donorConnectionString.toString());

    TenantMigrationAccessBlockerRegistry::get(serviceContext).add(tenantId, mtab);
}

boost::optional<std::string> parseTenantIdFromDB(StringData dbName) {
    auto pos = dbName.find("_");
    if (pos == std::string::npos || pos == 0) {
        // Not a tenant database.
        return boost::none;
    }

    return dbName.toString().substr(0, pos);
}

TenantMigrationDonorDocument parseDonorStateDocument(const BSONObj& doc) {
    auto donorStateDoc =
        TenantMigrationDonorDocument::parse(IDLParserContext("donorStateDoc"), doc);

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

SemiFuture<void> checkIfCanReadOrBlock(OperationContext* opCtx, const OpMsgRequest& request) {
    // We need to check both donor and recipient access blockers in the case where two
    // migrations happen back-to-back before the old recipient state (from the first
    // migration) is garbage collected.
    auto dbName = request.getDatabase();
    auto& blockerRegistry = TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext());
    auto mtabPair = blockerRegistry.getTenantMigrationAccessBlockerForDbName(dbName);

    if (!mtabPair) {
        return Status::OK();
    }

    // Source to cancel the timeout if the operation completed in time.
    CancellationSource cancelTimeoutSource;
    // Source to cancel waiting on the 'canReadFutures'.
    CancellationSource cancelCanReadSource(opCtx->getCancellationToken());
    const auto donorMtab = mtabPair->getAccessBlocker(MtabType::kDonor);
    const auto recipientMtab = mtabPair->getAccessBlocker(MtabType::kRecipient);
    // A vector of futures where the donor access blocker's 'getCanReadFuture' will always precede
    // the recipient's.
    std::vector<ExecutorFuture<void>> futures;
    std::shared_ptr<executor::TaskExecutor> executor;
    if (donorMtab) {
        auto canReadFuture = donorMtab->getCanReadFuture(opCtx, request.getCommandName());
        if (canReadFuture.isReady()) {
            auto status = canReadFuture.getNoThrow();
            donorMtab->recordTenantMigrationError(status);
            if (!recipientMtab) {
                return status;
            }
        }
        executor = blockerRegistry.getAsyncBlockingOperationsExecutor();
        futures.emplace_back(std::move(canReadFuture).semi().thenRunOn(executor));
    }
    if (recipientMtab) {
        auto canReadFuture = recipientMtab->getCanReadFuture(opCtx, request.getCommandName());
        if (canReadFuture.isReady()) {
            auto status = canReadFuture.getNoThrow();
            recipientMtab->recordTenantMigrationError(status);
            if (!donorMtab) {
                return status;
            }
        }
        executor = blockerRegistry.getAsyncBlockingOperationsExecutor();
        futures.emplace_back(std::move(canReadFuture).semi().thenRunOn(executor));
    }

    if (opCtx->hasDeadline()) {
        // Cancel waiting for operations if we timeout.
        executor->sleepUntil(opCtx->getDeadline(), cancelTimeoutSource.token())
            .getAsync([cancelCanReadSource](auto) mutable { cancelCanReadSource.cancel(); });
    }

    return future_util::withCancellation(whenAll(std::move(futures)), cancelCanReadSource.token())
        .thenRunOn(executor)
        .then([cancelTimeoutSource, donorMtab, recipientMtab](std::vector<Status> results) mutable {
            cancelTimeoutSource.cancel();
            auto resultIter = results.begin();
            const auto donorMtabStatus = donorMtab ? *resultIter++ : Status::OK();
            const auto recipientMtabStatus = recipientMtab ? *resultIter : Status::OK();
            if (!donorMtabStatus.isOK()) {
                donorMtab->recordTenantMigrationError(donorMtabStatus);
                LOGV2(5519301,
                      "Received error while waiting on donor access blocker",
                      "error"_attr = donorMtabStatus);
            }
            if (!recipientMtabStatus.isOK()) {
                recipientMtab->recordTenantMigrationError(recipientMtabStatus);
                LOGV2(5519302,
                      "Received error while waiting on recipient access blocker",
                      "error"_attr = recipientMtabStatus);
                if (donorMtabStatus.isOK()) {
                    return recipientMtabStatus;
                }
            }
            return donorMtabStatus;
        })
        .onError<ErrorCodes::CallbackCanceled>(
            [cancelTimeoutSource,
             cancelCanReadSource,
             donorMtab,
             recipientMtab,
             timeoutError = opCtx->getTimeoutError()](Status status) mutable {
                auto isCanceledDueToTimeout = cancelTimeoutSource.token().isCanceled();

                if (!isCanceledDueToTimeout) {
                    cancelTimeoutSource.cancel();
                }

                if (isCanceledDueToTimeout) {
                    return Status(timeoutError,
                                  "Blocked read timed out waiting for an internal data migration "
                                  "to commit or abort");
                }

                return status.withContext("Canceled read blocked by internal data migration");
            })
        .semi();  // To require continuation in the user executor.
}

void checkIfLinearizableReadWasAllowedOrThrow(OperationContext* opCtx, StringData dbName) {
    if (repl::ReadConcernArgs::get(opCtx).getLevel() ==
        repl::ReadConcernLevel::kLinearizableReadConcern) {
        // Only the donor access blocker will block linearizable reads.
        if (auto mtab = TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                            .getTenantMigrationAccessBlockerForDbName(dbName, MtabType::kDonor)) {
            auto status = mtab->checkIfLinearizableReadWasAllowed(opCtx);
            mtab->recordTenantMigrationError(status);
            uassertStatusOK(status);
        }
    }
}

void checkIfCanWriteOrThrow(OperationContext* opCtx, StringData dbName, Timestamp writeTs) {
    // The migration protocol guarantees the recipient will not get writes until the migration
    // is committed.
    auto mtab = TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                    .getTenantMigrationAccessBlockerForDbName(dbName, MtabType::kDonor);

    if (mtab) {
        auto status = mtab->checkIfCanWrite(writeTs);
        mtab->recordTenantMigrationError(status);
        uassertStatusOK(status);
    }
}

Status checkIfCanBuildIndex(OperationContext* opCtx, StringData dbName) {
    // We only block index builds on the donor.
    auto mtab = TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                    .getTenantMigrationAccessBlockerForDbName(dbName, MtabType::kDonor);

    if (mtab) {
        // This log is included for synchronization of the tenant migration buildindex jstests.
        auto status = mtab->checkIfCanBuildIndex();
        mtab->recordTenantMigrationError(status);

        if (MONGO_unlikely(haveCheckedIfIndexBuildableDuringTenantMigration.shouldFail())) {
            LOGV2(5835300,
                  "haveCheckedIfIndexBuildableDuringTenantMigration failpoint enabled",
                  "db"_attr = dbName,
                  "status"_attr = status);
        }

        return status;
    }
    return Status::OK();
}

bool hasActiveTenantMigration(OperationContext* opCtx, StringData dbName) {
    if (dbName.empty()) {
        return false;
    }

    return bool(TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                    .getTenantMigrationAccessBlockerForDbName(dbName));
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

        auto protocol = doc.getProtocol().value_or(MigrationProtocolEnum::kMultitenantMigrations);
        auto mtab = std::make_shared<TenantMigrationDonorAccessBlocker>(
            opCtx->getServiceContext(),
            doc.getId(),
            doc.getTenantId().toString(),
            protocol,
            doc.getRecipientConnectionString().toString());

        auto& registry = TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext());
        if (protocol == MigrationProtocolEnum::kMultitenantMigrations) {
            registry.add(doc.getTenantId(), mtab);
        } else {
            registry.addShardMergeDonorAccessBlocker(mtab);
        }

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
        // Do not create the mtab when:
        // 1) the migration was forgotten before receiving a 'recipientSyncData' with a
        //    'returnAfterReachingDonorTimestamp'.
        // 2) a delayed 'recipientForgetMigration' was received after the state doc was deleted.
        if (doc.getState() == TenantMigrationRecipientStateEnum::kDone &&
            !doc.getRejectReadsBeforeTimestamp()) {
            return true;
        }

        auto mtab = std::make_shared<TenantMigrationRecipientAccessBlocker>(
            opCtx->getServiceContext(),
            doc.getId(),
            doc.getTenantId().toString(),
            doc.getProtocol().value_or(MigrationProtocolEnum::kMultitenantMigrations),
            doc.getDonorConnectionString().toString());

        TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
            .add(doc.getTenantId(), mtab);

        switch (doc.getState()) {
            case TenantMigrationRecipientStateEnum::kStarted:
            case TenantMigrationRecipientStateEnum::kLearnedFilenames:
                invariant(!doc.getRejectReadsBeforeTimestamp());
                break;
            case TenantMigrationRecipientStateEnum::kConsistent:
            case TenantMigrationRecipientStateEnum::kDone:
                if (doc.getRejectReadsBeforeTimestamp()) {
                    mtab->startRejectingReadsBefore(doc.getRejectReadsBeforeTimestamp().get());
                }
                break;
            case TenantMigrationRecipientStateEnum::kUninitialized:
                MONGO_UNREACHABLE;
        }
        return true;
    });

    // Recover TenantMigrationDonorAccessBlockers for ShardSplit.
    PersistentTaskStore<ShardSplitDonorDocument> shardSplitDonorStore(
        NamespaceString::kShardSplitDonorsNamespace);

    shardSplitDonorStore.forEach(opCtx, {}, [&](const ShardSplitDonorDocument& doc) {
        // Skip creating a TenantMigrationDonorAccessBlocker for terminal shard split that have been
        // marked as garbage collected.
        if (doc.getExpireAt() &&
            (doc.getState() == ShardSplitDonorStateEnum::kCommitted ||
             doc.getState() == ShardSplitDonorStateEnum::kAborted)) {
            return true;
        }

        auto optionalTenants = doc.getTenantIds();
        invariant(optionalTenants);
        for (const auto& tenantId : optionalTenants.get()) {
            invariant(doc.getRecipientConnectionString());
            auto mtab = std::make_shared<TenantMigrationDonorAccessBlocker>(
                opCtx->getServiceContext(),
                doc.getId(),
                tenantId.toString(),
                MigrationProtocolEnum::kMultitenantMigrations,
                doc.getRecipientConnectionString()->toString());
            TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                .add(tenantId.toString(), mtab);

            switch (doc.getState()) {
                case ShardSplitDonorStateEnum::kAbortingIndexBuilds:
                    break;
                case ShardSplitDonorStateEnum::kBlocking:
                    invariant(doc.getBlockTimestamp());
                    mtab->startBlockingWrites();
                    mtab->startBlockingReadsAfter(doc.getBlockTimestamp().get());
                    break;
                case ShardSplitDonorStateEnum::kCommitted:
                    invariant(doc.getBlockTimestamp());
                    mtab->startBlockingWrites();
                    mtab->startBlockingReadsAfter(doc.getBlockTimestamp().get());
                    mtab->setCommitOpTime(opCtx, doc.getCommitOrAbortOpTime().get());
                    break;
                case ShardSplitDonorStateEnum::kAborted:
                    if (doc.getBlockTimestamp()) {
                        mtab->startBlockingWrites();
                        mtab->startBlockingReadsAfter(doc.getBlockTimestamp().get());
                    }
                    mtab->setAbortOpTime(opCtx, doc.getCommitOrAbortOpTime().get());
                    break;
                case ShardSplitDonorStateEnum::kUninitialized:
                    MONGO_UNREACHABLE;
            }
        }
        return true;
    });
}

template <typename MigrationConflictInfoType>
Status _handleTenantMigrationConflict(OperationContext* opCtx, const Status& status) {
    auto migrationConflictInfo = status.extraInfo<MigrationConflictInfoType>();
    invariant(migrationConflictInfo);
    auto mtab = migrationConflictInfo->getTenantMigrationAccessBlocker();
    invariant(mtab);
    auto migrationStatus = mtab->waitUntilCommittedOrAborted(opCtx);
    mtab->recordTenantMigrationError(migrationStatus);
    return migrationStatus;
}

Status handleTenantMigrationConflict(OperationContext* opCtx, Status status) {
    if (status == ErrorCodes::NonRetryableTenantMigrationConflict) {
        auto migrationStatus =
            _handleTenantMigrationConflict<NonRetryableTenantMigrationConflictInfo>(opCtx, status);

        // Some operations, like multi updates, can't safely be automatically retried so we return a
        // non retryable error instead of TenantMigrationCommitted/TenantMigrationAborted. If
        // waiting failed for a different reason, e.g. MaxTimeMS expiring, propagate that to the
        // user unchanged.
        if (ErrorCodes::isTenantMigrationError(migrationStatus)) {
            return kNonRetryableTenantMigrationStatus;
        }
        return migrationStatus;
    }

    return _handleTenantMigrationConflict<TenantMigrationConflictInfo>(opCtx, status);
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

    auto memberState = replCoord->getMemberState();

    return memberState.startup() || memberState.startup2() || memberState.rollback();
}

}  // namespace tenant_migration_access_blocker

}  // namespace mongo
