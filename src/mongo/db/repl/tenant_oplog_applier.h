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

#pragma once

#include <string>
#include <vector>

#include "mongo/db/repl/abstract_async_component.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_buffer.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/tenant_oplog_batcher.h"
#include "mongo/util/future.h"

namespace mongo {
class ThreadPool;

namespace repl {

/**
 * This class reads oplog entries from a tenant migration, then applies those entries to the
 * (real) oplog, then writes out no-op entries corresponding to the original oplog entries
 * from the oplog buffer.  Applier will not apply, but will write no-op entries for,
 * entries before the applyFromOpTime.
 *
 */
class TenantOplogApplier : public AbstractAsyncComponent,
                           public std::enable_shared_from_this<TenantOplogApplier> {
public:
    struct OpTimePair {
        OpTimePair() = default;
        OpTimePair(OpTime in_donorOpTime, OpTime in_recipientOpTime)
            : donorOpTime(in_donorOpTime), recipientOpTime(in_recipientOpTime) {}
        bool operator<(const OpTimePair& other) const {
            if (donorOpTime == other.donorOpTime)
                return recipientOpTime < other.recipientOpTime;
            return donorOpTime < other.donorOpTime;
        }
        std::string toString() const {
            return BSON("donorOpTime" << donorOpTime << "recipientOpTime" << recipientOpTime)
                .toString();
        }
        OpTime donorOpTime;
        OpTime recipientOpTime;
    };

    TenantOplogApplier(const UUID& migrationUuid,
                       const std::string& tenantId,
                       OpTime applyFromOpTime,
                       RandomAccessOplogBuffer* oplogBuffer,
                       std::shared_ptr<executor::TaskExecutor> executor,
                       ThreadPool* writerPool,
                       const bool isResuming = false);

    virtual ~TenantOplogApplier();

    /**
     * Return a future which will be notified when that optime has been reached.  Future will
     * contain donor and recipient optime of last oplog entry in batch where donor optime is greater
     * than passed-in time. To be noted, recipient optime returned in the future can be null if the
     * tenant oplog applier has never applied any tenant oplog entries (i.e., non resume token no-op
     * entries) till that batch.
     */
    SemiFuture<OpTimePair> getNotificationForOpTime(OpTime donorOpTime);

    /**
     * Returns the optime the applier will start applying from. Used for testing.
     */
    OpTime getBeginApplyingOpTime_forTest() const;

private:
    Status _doStartup_inlock() noexcept final;
    void _doShutdown_inlock() noexcept final;
    void _preJoin() noexcept final;
    void _finishShutdown(WithLock lk, Status status);

    void _applyLoop(TenantOplogBatch batch);
    bool _shouldStopApplying(Status status);

    void _applyOplogBatch(TenantOplogBatch* batch);
    Status _applyOplogBatchPerWorker(std::vector<const OplogEntry*>* ops);
    void _checkNsAndUuidsBelongToTenant(OperationContext* opCtx, const TenantOplogBatch& batch);
    OpTimePair _writeNoOpEntries(OperationContext* opCtx, const TenantOplogBatch& batch);
    void _writeNoOpsForRange(OpObserver* opObserver,
                             std::vector<TenantOplogEntry>::const_iterator begin,
                             std::vector<TenantOplogEntry>::const_iterator end,
                             std::vector<OplogSlot>::iterator firstSlot);

    Status _applyOplogEntryOrGroupedInserts(OperationContext* opCtx,
                                            const OplogEntryOrGroupedInserts& entryOrGroupedInserts,
                                            OplogApplication::Mode oplogApplicationMode);
    std::vector<std::vector<const OplogEntry*>> _fillWriterVectors(OperationContext* opCtx,
                                                                   TenantOplogBatch* batch);

    OpTime _getRecipientOpTime(const OpTime& donorOpTime);
    // This is a convenience call for getRecipientOpTime which handles boost::none and nulls.
    boost::optional<OpTime> _maybeGetRecipientOpTime(const boost::optional<OpTime>);
    // _setRecipientOpTime must be called in optime order.
    void _setRecipientOpTime(const OpTime& donorOpTime, const OpTime& recipientOpTime);
    /**
     * Sets the _finalStatus to the new status if and only if the old status is "OK".
     */
    void _setFinalStatusIfOk(WithLock, Status newStatus);

    Mutex* _getMutex() noexcept final {
        return &_mutex;
    }

    Mutex _mutex = MONGO_MAKE_LATCH("TenantOplogApplier::_mutex");
    // All member variables are labeled with one of the following codes indicating the
    // synchronization rules for accessing them.
    //
    // (R)  Read-only in concurrent operation; no synchronization required.
    // (S)  Self-synchronizing; access according to class's own rules.
    // (M)  Reads and writes guarded by _mutex
    // (X)  Access only allowed from the main flow of control called from run() or constructor.

    // Handles consuming oplog entries from the OplogBuffer for oplog application.
    std::shared_ptr<TenantOplogBatcher> _oplogBatcher;  // (R)
    const UUID _migrationUuid;                          // (R)
    const std::string _tenantId;                        // (R)
    const OpTime _beginApplyingAfterOpTime;             // (R)
    RandomAccessOplogBuffer* _oplogBuffer;              // (R)
    std::shared_ptr<executor::TaskExecutor> _executor;  // (R)
    // Keeps track of last applied donor and recipient optimes by the tenant oplog applier.
    // This gets updated only on batch boundaries.
    OpTimePair _lastAppliedOpTimesUpToLastBatch;  // (M)
    std::vector<OpTimePair> _opTimeMapping;       // (M)
    // Pool of worker threads for writing ops to the databases.
    // Not owned by us.
    ThreadPool* const _writerPool;                                        // (S)
    std::map<OpTime, SharedPromise<OpTimePair>> _opTimeNotificationList;  // (M)
    Status _finalStatus = Status::OK();                                   // (M)
    stdx::unordered_set<UUID, UUID::Hash> _knownGoodUuids;                // (X)
    bool _applyLoopApplyingBatch = false;                                 // (M)
    const bool _isResuming;                                               // (R)
};

/**
 * Creates the default thread pool for writer tasks.
 */
std::unique_ptr<ThreadPool> makeTenantMigrationWriterPool();
std::unique_ptr<ThreadPool> makeTenantMigrationWriterPool(int threadCount);

}  // namespace repl
}  // namespace mongo
