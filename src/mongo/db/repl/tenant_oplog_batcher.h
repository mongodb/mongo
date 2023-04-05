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
#include "mongo/db/repl/oplog_buffer.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/util/future.h"

namespace mongo {
namespace repl {

struct TenantOplogEntry {
    TenantOplogEntry() = default;
    explicit TenantOplogEntry(OplogEntry&& in_entry) : entry(in_entry) {}
    TenantOplogEntry(OplogEntry&& in_entry, int in_expansionsEntry)
        : entry(in_entry), expansionsEntry(in_expansionsEntry) {}

    OplogEntry entry;
    // If this entry is a transaction commit or applyOps, the index within the
    // TenantOplogBatch::expansions array containing the oplog entries it expands to.
    int expansionsEntry = -1;
    // When the fetched donor oplog entry represents modifications to internal collections
    // (i.e, collections in admin/config DBs), shard merge protocol skips applying those oplog
    // entries. In those cases, we set this field to 'true' to indicate that the tenant oplog
    // applier can skip writing no-op entries for this oplog entry.
    bool ignore = false;
};

struct TenantOplogBatch {
    std::vector<TenantOplogEntry> ops;
    std::vector<std::vector<OplogEntry>> expansions;
};

/**
 * This class consumes batches of oplog entries from the RandomAccessOplogBuffer to give to the
 * tenant oplog applier.  It expands transactions into their individual ops and keeps them together
 * in a single batch.  The original transaction information is included in the batch.
 */
class TenantOplogBatcher : public AbstractAsyncComponent,
                           public std::enable_shared_from_this<TenantOplogBatcher> {
public:
    class BatchLimits {
    public:
        BatchLimits() = default;
        BatchLimits(size_t in_bytes, size_t in_ops) : bytes(in_bytes), ops(in_ops) {}
        size_t bytes = 0;
        size_t ops = 0;
    };

    TenantOplogBatcher(const UUID& migrationUuid,
                       RandomAccessOplogBuffer* oplogBuffer,
                       std::shared_ptr<executor::TaskExecutor> executor,
                       Timestamp resumeBatchingTs,
                       OpTime beginApplyingAfterOpTime);

    virtual ~TenantOplogBatcher();

    /**
     * Returns a future for the next oplog batch. Client must not ask for another batch until
     * the Future is ready.
     */
    SemiFuture<TenantOplogBatch> getNextBatch(BatchLimits limits);

private:
    SemiFuture<TenantOplogBatch> _scheduleNextBatch(WithLock, BatchLimits limits);

    StatusWith<TenantOplogBatch> _readNextBatch(BatchLimits limits);

    bool _mustProcessIndividually(const OplogEntry& entry);

    void _consume(OperationContext* opCtx);

    void _pushEntry(OperationContext* opCtx, TenantOplogBatch* batch, OplogEntry&& op);

    void _doStartup_inlock() final;

    void _doShutdown_inlock() noexcept final;

    void _preJoin() noexcept final {}

    Mutex* _getMutex() noexcept final {
        return &_mutex;
    }

    Mutex _mutex = MONGO_MAKE_LATCH("TenantOplogBatcher::_mutex");
    // All member variables are labeled with one of the following codes indicating the
    // synchronization rules for accessing them.
    //
    // (R)  Read-only in concurrent operation; no synchronization required.
    // (S)  Self-synchronizing; access according to class's own rules.
    // (M)  Reads and writes guarded by _mutex
    // (X)  Access only allowed from the main flow of control called from run() or constructor.

    RandomAccessOplogBuffer* _oplogBuffer;              // (S)
    bool _batchRequested = false;                       // (M)
    std::shared_ptr<executor::TaskExecutor> _executor;  // (R)
    const Timestamp _resumeBatchingTs;                  // (R)
    const OpTime _beginApplyingAfterOpTime;             // (R)
};

}  // namespace repl
}  // namespace mongo
