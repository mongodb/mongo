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

#include "mongo/db/repl/tenant_oplog_applier.h"

#include <algorithm>

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/repl/apply_ops.h"
#include "mongo/db/repl/cloner_utils.h"
#include "mongo/db/repl/insert_group.h"
#include "mongo/db/repl/oplog_applier_utils.h"
#include "mongo/db/repl/tenant_migration_decoration.h"
#include "mongo/db/repl/tenant_oplog_batcher.h"
#include "mongo/logv2/log.h"
#include "mongo/util/concurrency/thread_pool.h"

namespace mongo {
namespace repl {

// These batch sizes are pretty arbitrary.
// TODO(SERVER-50024): come up with some reasonable numbers, and make them a settable parameter.
constexpr size_t kTenantApplierBatchSizeBytes = 16 * 1024 * 1024;

// TODO(SERVER-50024): kTenantApplierBatchSizeOps is currently chosen as the default value of
// internalInsertMaxBatchSize.  This is probably reasonable but should be a settable parameter.
constexpr size_t kTenantApplierBatchSizeOps = 500;
const size_t kMinOplogEntriesPerThread = 16;

// TODO(SERVER-50024):: This is also arbitary and should be a settable parameter.
constexpr size_t kTenantApplierThreadCount = 5;

TenantOplogApplier::TenantOplogApplier(const UUID& migrationUuid,
                                       const std::string& tenantId,
                                       OpTime applyFromOpTime,
                                       RandomAccessOplogBuffer* oplogBuffer,
                                       std::shared_ptr<executor::TaskExecutor> executor,
                                       ThreadPool* writerPool)
    : AbstractAsyncComponent(executor.get(), std::string("TenantOplogApplier_") + tenantId),
      _migrationUuid(migrationUuid),
      _tenantId(tenantId),
      _beginApplyingAfterOpTime(applyFromOpTime),
      _oplogBuffer(oplogBuffer),
      _executor(std::move(executor)),
      _writerPool(writerPool),
      _limits(kTenantApplierBatchSizeBytes, kTenantApplierBatchSizeOps) {}

TenantOplogApplier::~TenantOplogApplier() {
    shutdown();
    join();
}

SemiFuture<TenantOplogApplier::OpTimePair> TenantOplogApplier::getNotificationForOpTime(
    OpTime donorOpTime) {
    stdx::lock_guard lk(_mutex);
    // If we're not running, return a future with the status we shut down with.
    if (!_isActive_inlock()) {
        return SemiFuture<OpTimePair>::makeReady(_finalStatus);
    }
    // If this optime has already passed, just return a ready future.
    if (_lastBatchCompletedOpTimes.donorOpTime >= donorOpTime) {
        return SemiFuture<OpTimePair>::makeReady(_lastBatchCompletedOpTimes);
    }

    // This will pull a new future off the existing promise for this time if it exists, otherwise
    // it constructs a new promise and pulls a future off of it.
    auto [iter, isNew] = _opTimeNotificationList.try_emplace(donorOpTime);
    return iter->second.getFuture().semi();
}

Status TenantOplogApplier::_doStartup_inlock() noexcept {
    _oplogBatcher = std::make_unique<TenantOplogBatcher>(_tenantId, _oplogBuffer, _executor);
    auto status = _oplogBatcher->startup();
    if (!status.isOK())
        return status;
    auto fut = _oplogBatcher->getNextBatch(_limits);
    std::move(fut)
        .thenRunOn(_executor)
        .then([&](TenantOplogBatch batch) { _applyLoop(std::move(batch)); })
        .onError([&](Status status) { _handleError(status); })
        .getAsync([](auto status) {});
    return Status::OK();
}

void TenantOplogApplier::_doShutdown_inlock() noexcept {
    // Shutting down the oplog batcher will make the _applyLoop stop with an error future, thus
    // shutting down the applier.
    _oplogBatcher->shutdown();
}

void TenantOplogApplier::_applyLoop(TenantOplogBatch batch) {
    // Getting the future for the next batch here means the batcher can retrieve the next batch
    // while the applier is processing the current one.
    auto nextBatchFuture = _oplogBatcher->getNextBatch(_limits);
    try {
        _applyOplogBatch(&batch);
    } catch (const DBException& e) {
        _handleError(e.toStatus());
        return;
    }
    {
        stdx::lock_guard lk(_mutex);
        if (_isShuttingDown_inlock()) {
            _finishShutdown(lk, {ErrorCodes::CallbackCanceled, "Tenant Oplog Applier shut down"});
            return;
        }
    }
    std::move(nextBatchFuture)
        .thenRunOn(_executor)
        .then([&](TenantOplogBatch batch) { _applyLoop(std::move(batch)); })
        .onError([&](Status status) { _handleError(status); })
        .getAsync([](auto status) {});
}

void TenantOplogApplier::_handleError(Status status) {
    LOGV2_DEBUG(4886005,
                1,
                "TenantOplogApplier::_handleError",
                "tenant"_attr = _tenantId,
                "migrationUuid"_attr = _migrationUuid,
                "error"_attr = redact(status));
    shutdown();
    stdx::lock_guard lk(_mutex);
    // If we reach _handleError, it means the applyLoop is not running.
    _finishShutdown(lk, status);
}

void TenantOplogApplier::_finishShutdown(WithLock, Status status) {
    // Any unfulfilled notifications are errored out.
    for (auto& listEntry : _opTimeNotificationList) {
        listEntry.second.setError(status);
    }
    _opTimeNotificationList.clear();
    _finalStatus = status;
    _transitionToComplete_inlock();
}

void TenantOplogApplier::_applyOplogBatch(TenantOplogBatch* batch) {
    LOGV2_DEBUG(4886004,
                1,
                "Tenant Oplog Applier starting to apply batch",
                "tenant"_attr = _tenantId,
                "migrationUuid"_attr = _migrationUuid,
                "firstDonorOptime"_attr = batch->ops.front().entry.getOpTime(),
                "lastDonorOptime"_attr = batch->ops.back().entry.getOpTime());
    auto opCtx = cc().makeOperationContext();
    _checkNsAndUuidsBelongToTenant(opCtx.get(), *batch);
    auto writerVectors = _fillWriterVectors(opCtx.get(), batch);
    std::vector<Status> statusVector(writerVectors.size(), Status::OK());
    for (size_t i = 0; i < writerVectors.size(); i++) {
        if (writerVectors[i].empty())
            continue;

        _writerPool->schedule([this, &writer = writerVectors.at(i), &status = statusVector.at(i)](
                                  auto scheduleStatus) {
            if (!scheduleStatus.isOK()) {
                status = scheduleStatus;
            } else {
                status = _applyOplogBatchPerWorker(&writer);
            }
        });
    }
    _writerPool->waitForIdle();

    // Make sure all the workers succeeded.
    for (const auto& status : statusVector) {
        if (!status.isOK()) {
            LOGV2_ERROR(4886012,
                        "Failed to apply operation in tenant migration",
                        "tenant"_attr = _tenantId,
                        "migrationUuid"_attr = _migrationUuid,
                        "error"_attr = redact(status));
        }
        uassertStatusOK(status);
    }


    LOGV2_DEBUG(4886011,
                1,
                "Tenant Oplog Applier starting to write no-ops",
                "tenant"_attr = _tenantId,
                "migrationUuid"_attr = _migrationUuid);
    auto lastBatchCompletedOpTimes = _writeNoOpEntries(opCtx.get(), *batch);
    stdx::lock_guard lk(_mutex);
    _lastBatchCompletedOpTimes = lastBatchCompletedOpTimes;
    LOGV2_DEBUG(4886002,
                1,
                "Tenant Oplog Applier finished applying batch",
                "tenant"_attr = _tenantId,
                "migrationUuid"_attr = _migrationUuid,
                "lastDonorOptime"_attr = lastBatchCompletedOpTimes.donorOpTime,
                "lastRecipientOptime"_attr = lastBatchCompletedOpTimes.recipientOpTime);

    // Notify all the waiters on optimes before and including _lastBatchCompletedOpTimes.
    auto firstUnexpiredIter =
        _opTimeNotificationList.upper_bound(_lastBatchCompletedOpTimes.donorOpTime);
    for (auto iter = _opTimeNotificationList.begin(); iter != firstUnexpiredIter; iter++) {
        iter->second.emplaceValue(_lastBatchCompletedOpTimes);
    }
    _opTimeNotificationList.erase(_opTimeNotificationList.begin(), firstUnexpiredIter);
}

void TenantOplogApplier::_checkNsAndUuidsBelongToTenant(OperationContext* opCtx,
                                                        const TenantOplogBatch& batch) {
    auto checkNsAndUuid = [&](const OplogEntry& op) {
        if (!op.getNss().isEmpty() && !ClonerUtils::isNamespaceForTenant(op.getNss(), _tenantId)) {
            LOGV2_ERROR(4886015,
                        "Namespace does not belong to tenant being migrated",
                        "tenant"_attr = _tenantId,
                        "migrationUuid"_attr = _migrationUuid,
                        "nss"_attr = op.getNss());
            uasserted(4886016, "Namespace does not belong to tenant being migrated");
        }
        if (!op.getUuid())
            return;
        if (_knownGoodUuids.find(*op.getUuid()) != _knownGoodUuids.end())
            return;
        try {
            auto nss = OplogApplierUtils::parseUUIDOrNs(opCtx, op);
            if (!ClonerUtils::isNamespaceForTenant(nss, _tenantId)) {
                LOGV2_ERROR(4886013,
                            "UUID does not belong to tenant being migrated",
                            "tenant"_attr = _tenantId,
                            "migrationUuid"_attr = _migrationUuid,
                            "UUID"_attr = *op.getUuid(),
                            "nss"_attr = nss.ns());
                uasserted(4886014, "UUID does not belong to tenant being migrated");
            }
            _knownGoodUuids.insert(*op.getUuid());
        } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
            LOGV2_DEBUG(4886017,
                        2,
                        "UUID for tenant being migrated does not exist",
                        "tenant"_attr = _tenantId,
                        "migrationUuid"_attr = _migrationUuid,
                        "UUID"_attr = *op.getUuid(),
                        "nss"_attr = op.getNss().ns());
        }
    };

    for (const auto& op : batch.ops) {
        if (op.expansionsEntry < 0 && !op.entry.isPartialTransaction())
            checkNsAndUuid(op.entry);
    }
    for (const auto& expansion : batch.expansions) {
        for (const auto& op : expansion) {
            checkNsAndUuid(op);
        }
    }
}

TenantOplogApplier::OpTimePair TenantOplogApplier::_writeNoOpEntries(
    OperationContext* opCtx, const TenantOplogBatch& batch) {
    auto* opObserver = cc().getServiceContext()->getOpObserver();

    WriteUnitOfWork wuow(opCtx);
    // Reserve oplog slots for all entries.  This allows us to write them in parallel.
    auto oplogSlots = repl::getNextOpTimes(opCtx, batch.ops.size());
    auto slotIter = oplogSlots.begin();
    for (const auto& op : batch.ops) {
        _setRecipientOpTime(op.entry.getOpTime(), *slotIter++);
    }
    const size_t numOplogThreads = _writerPool->getStats().numThreads;
    const size_t numOpsPerThread =
        std::max(kMinOplogEntriesPerThread, (batch.ops.size() / numOplogThreads));
    slotIter = oplogSlots.begin();
    auto opsIter = batch.ops.begin();
    LOGV2_DEBUG(4886003,
                1,
                "Tenant Oplog Applier scheduling no-ops ",
                "tenant"_attr = _tenantId,
                "migrationUuid"_attr = _migrationUuid,
                "firstDonorOptime"_attr = batch.ops.front().entry.getOpTime(),
                "lastDonorOptime"_attr = batch.ops.back().entry.getOpTime(),
                "numOplogThreads"_attr = numOplogThreads,
                "numOpsPerThread"_attr = numOpsPerThread);
    size_t numOpsRemaining = batch.ops.size();
    for (size_t thread = 0; thread < numOplogThreads && opsIter != batch.ops.end(); thread++) {
        auto numOps = std::min(numOpsPerThread, numOpsRemaining);
        if (thread == numOplogThreads - 1) {
            numOps = numOpsRemaining;
        }
        _writerPool->schedule([=](Status status) {
            invariant(status);
            _writeNoOpsForRange(opObserver, opsIter, opsIter + numOps, slotIter);
        });
        slotIter += numOps;
        opsIter += numOps;
        numOpsRemaining -= numOps;
    }
    invariant(opsIter == batch.ops.end());
    _writerPool->waitForIdle();
    return {batch.ops.back().entry.getOpTime(), oplogSlots.back()};
}


// These two routines can't be the ultimate solution.  It's not necessarily practical to keep a list
// of every op we've written, and it doesn't work for failover.  But as far as I can tell, it's
// possible to refer to oplog entries arbitarily far back.  We probably don't want to search the
// oplog each time because it requires a collection scan to do so.
// TODO(SERVER-50263): Come up with the right way to do this.
OpTime TenantOplogApplier::_getRecipientOpTime(const OpTime& donorOpTime) {
    stdx::lock_guard lk(_mutex);
    auto times = std::upper_bound(
        _opTimeMapping.begin(), _opTimeMapping.end(), OpTimePair(donorOpTime, OpTime()));
    uassert(4886000,
            str::stream() << "Recipient optime not found for donor optime "
                          << donorOpTime.toString(),
            times->donorOpTime == donorOpTime);
    return times->recipientOpTime;
}

void TenantOplogApplier::_setRecipientOpTime(const OpTime& donorOpTime,
                                             const OpTime& recipientOpTime) {
    stdx::lock_guard lk(_mutex);
    // The _opTimeMapping is an array strictly ordered by donorOpTime; this uassert assures the
    // order remains intact.
    uassert(4886001,
            str::stream() << "Donor optimes inserted out of order "
                          << _opTimeMapping.back().donorOpTime.toString()
                          << " >= " << donorOpTime.toString(),
            _opTimeMapping.empty() || _opTimeMapping.back().donorOpTime < donorOpTime);
    _opTimeMapping.emplace_back(donorOpTime, recipientOpTime);
}

boost::optional<OpTime> TenantOplogApplier::_maybeGetRecipientOpTime(
    const boost::optional<OpTime> donorOpTime) {
    if (!donorOpTime || donorOpTime->isNull())
        return donorOpTime;
    return _getRecipientOpTime(*donorOpTime);
}

void TenantOplogApplier::_writeNoOpsForRange(OpObserver* opObserver,
                                             std::vector<TenantOplogEntry>::const_iterator begin,
                                             std::vector<TenantOplogEntry>::const_iterator end,
                                             std::vector<OplogSlot>::iterator firstSlot) {
    auto opCtx = cc().makeOperationContext();
    tenantMigrationRecipientInfo(opCtx.get()) =
        boost::make_optional<TenantMigrationRecipientInfo>(_migrationUuid);
    AutoGetOplog oplogWrite(opCtx.get(), OplogAccessMode::kWrite);
    writeConflictRetry(
        opCtx.get(), "writeTenantNoOps", NamespaceString::kRsOplogNamespace.ns(), [&] {
            WriteUnitOfWork wuow(opCtx.get());
            auto slot = firstSlot;
            for (auto iter = begin; iter != end; iter++, slot++) {
                opObserver->onInternalOpMessage(
                    opCtx.get(),
                    iter->entry.getNss(),
                    iter->entry.getUuid(),
                    iter->entry.toBSON(),
                    BSONObj(),
                    // We link the no-ops together by recipient op time the same way the actual ops
                    // were linked together by donor op time.  This is to allow retryable writes
                    // and changestreams to find the ops they need.
                    _maybeGetRecipientOpTime(iter->entry.getPreImageOpTime()),
                    _maybeGetRecipientOpTime(iter->entry.getPostImageOpTime()),
                    _maybeGetRecipientOpTime(iter->entry.getPrevWriteOpTimeInTransaction()),
                    *slot);
            }
        });
}

std::vector<std::vector<const OplogEntry*>> TenantOplogApplier::_fillWriterVectors(
    OperationContext* opCtx, TenantOplogBatch* batch) {
    std::vector<std::vector<const OplogEntry*>> writerVectors(_writerPool->getStats().numThreads);
    CachedCollectionProperties collPropertiesCache;

    for (auto&& op : batch->ops) {
        // If the operation's optime is before or the same as the beginApplyingAfterOpTime we don't
        // want to apply it, so don't include it in writerVectors.
        if (op.entry.getOpTime() <= _beginApplyingAfterOpTime)
            continue;
        uassert(4886006,
                "Tenant oplog application does not support prepared transactions.",
                !op.entry.shouldPrepare());
        uassert(4886007,
                "Tenant oplog application does not support prepared transactions.",
                !op.entry.isPreparedCommit());

        // We never need to apply no-ops or partial transactions.
        if (op.entry.getOpType() == OpTypeEnum::kNoop || op.entry.isPartialTransaction())
            continue;

        if (op.expansionsEntry >= 0) {
            // This is an applyOps or transaction; add the expansions to the writer vectors.
            OplogApplierUtils::addDerivedOps(opCtx,
                                             &batch->expansions[op.expansionsEntry],
                                             &writerVectors,
                                             &collPropertiesCache,
                                             false /* serial */);
        } else {
            // Add a single op to the writer vectors.
            OplogApplierUtils::addToWriterVector(
                opCtx, &op.entry, &writerVectors, &collPropertiesCache);
        }
    }
    return writerVectors;
}

Status TenantOplogApplier::_applyOplogEntryOrGroupedInserts(
    OperationContext* opCtx,
    const OplogEntryOrGroupedInserts& entryOrGroupedInserts,
    OplogApplication::Mode oplogApplicationMode) {
    // We must ensure the opCtx uses replicated writes, because that will ensure we get a
    // NotWritablePrimary error if a stepdown occurs.
    invariant(opCtx->writesAreReplicated());

    // Ensure context matches that of _applyOplogBatchPerWorker.
    invariant(oplogApplicationMode == OplogApplication::Mode::kInitialSync);

    auto op = entryOrGroupedInserts.getOp();
    if (op.isIndexCommandType()) {
        // TODO(SERVER-48862): Handle index builds during oplog application.
        LOGV2_ERROR(488610,
                    "Index operations are not currently supported in tenant migration",
                    "tenant"_attr = _tenantId,
                    "migrationUuid"_attr = _migrationUuid,
                    "op"_attr = redact(op.toBSON()));

        return Status::OK();
    }
    // We don't count tenant application in the ops applied stats.
    auto incrementOpsAppliedStats = [] {};
    // We always use oplog application mode 'kInitialSync', because we're applying oplog entries to
    // a cloned database the way initial sync does.
    auto status = OplogApplierUtils::applyOplogEntryOrGroupedInsertsCommon(
        opCtx,
        entryOrGroupedInserts,
        OplogApplication::Mode::kInitialSync,
        incrementOpsAppliedStats,
        nullptr /* opCounters*/);
    LOGV2_DEBUG(4886009,
                2,
                "Applied tenant operation",
                "tenant"_attr = _tenantId,
                "migrationUuid"_attr = _migrationUuid,
                "error"_attr = status,
                "op"_attr = redact(op.toBSON()));
    return status;
}

Status TenantOplogApplier::_applyOplogBatchPerWorker(std::vector<const OplogEntry*>* ops) {
    auto opCtx = cc().makeOperationContext();
    tenantMigrationRecipientInfo(opCtx.get()) =
        boost::make_optional<TenantMigrationRecipientInfo>(_migrationUuid);

    const bool allowNamespaceNotFoundErrorsOnCrudOps(true);
    auto status = OplogApplierUtils::applyOplogBatchCommon(
        opCtx.get(),
        ops,
        OplogApplication::Mode::kInitialSync,
        allowNamespaceNotFoundErrorsOnCrudOps,
        [this](OperationContext* opCtx,
               const OplogEntryOrGroupedInserts& opOrInserts,
               OplogApplication::Mode mode) {
            return _applyOplogEntryOrGroupedInserts(opCtx, opOrInserts, mode);
        });
    if (!status.isOK()) {
        LOGV2_ERROR(4886008,
                    "Tenant migration writer worker batch application failed",
                    "tenant"_attr = _tenantId,
                    "migrationUuid"_attr = _migrationUuid,
                    "error"_attr = redact(status));
    }
    return status;
}

std::unique_ptr<ThreadPool> makeTenantMigrationWriterPool() {
    return makeTenantMigrationWriterPool(kTenantApplierThreadCount);
}

std::unique_ptr<ThreadPool> makeTenantMigrationWriterPool(int threadCount) {
    return makeReplWriterPool(
        threadCount, "TenantMigrationWriter"_sd, true /*  isKillableByStepdown */);
}

}  // namespace repl
}  // namespace mongo
