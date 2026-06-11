/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/base/status.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/user_write_block/replica_set_writes_block_reason_gen.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/modules.h"

#include <array>
#include <atomic>
#include <cstdint>

#include <boost/optional.hpp>

namespace mongo {

// Identifies the kind of write rejected by checkReplicaSetWritesAllowed so the matching counter
// can be incremented. Deletes are not included since they are validated separately by
// checkReplicaSetDeletionsAllowed and tracked through their own counter.
// TODO (SERVER-125476): Change the enum modularity to PRIVATE
enum class MONGO_MOD_NEEDS_REPLACEMENT ReplicaSetWriteBlockRejectedWriteOp { kInsert, kUpdate };

// TODO (SERVER-125476): Change the class modularity to PRIVATE
class MONGO_MOD_NEEDS_REPLACEMENT ReplicaSetWriteBlockState {
public:
    ReplicaSetWriteBlockState() = default;

    static ReplicaSetWriteBlockState* get(ServiceContext* serviceContext);
    static ReplicaSetWriteBlockState* get(OperationContext* opCtx);

    /**
     * Methods to control the replica set write blocking state.
     */
    void enableReplicaSetWriteBlocking(ReplicaSetWritesBlockReasonEnum reason);
    void disableReplicaSetWriteBlocking();

    /**
     * Gets the reason why the replica set writes are blocked.
     */
    boost::optional<int> getReplicaSetWriteBlockingReason(OperationContext* opCtx) const {
        const auto info = _writeBlockInfo.load();
        if (!info.blocked) {
            return boost::none;
        }
        return static_cast<int>(info.reason);
    }

    /**
     * Checks that replica set writes are allowed on the specified namespace. Throws
     * ReplicaSetWritesBlocked if user writes are disallowed.
     */
    void checkReplicaSetWritesAllowed(OperationContext* opCtx,
                                      const NamespaceString& nss,
                                      ReplicaSetWriteBlockRejectedWriteOp opKind) const;

    /**
     * Returns whether replica set write blocking is enabled, disregarding a specific namespace
     * and the state of WriteBlockBypass. Used for serverStatus.
     */
    bool isReplicaSetWriteBlockingEnabled() const;

    /**
     * Checks that a new compact operation is allowed to start on this replica set. Returns a
     * Status with ErrorCodes::ReplicaSetWritesBlocked if compact is disallowed, Status::OK()
     * otherwise.
     */
    Status checkIfCompactAllowedToStart(OperationContext* opCtx) const;

    /**
     * Checks that a new convertToCapped operation is allowed to start on the specified namespace.
     * Returns a Status with ErrorCodes::ReplicaSetWritesBlocked if replica set writes are blocked,
     * Status::OK() otherwise.
     */
    Status checkIfConvertToCappedAllowedToStart(OperationContext* opCtx,
                                                const NamespaceString& nss) const;

    /**
     * Methods to control the replica set deletions blocking state.
     */
    void enableReplicaSetDeletionsBlocking();
    void disableReplicaSetDeletionsBlocking();

    /**
     * Checks that replica set deletions are allowed on the specified namespace. Throws
     * ReplicaSetWritesBlocked if deletions are disallowed.
     */
    void checkReplicaSetDeletionsAllowed(OperationContext* opCtx, const NamespaceString& nss) const;

    /**
     * Returns whether replica set deletions blocking is enabled, disregarding a specific namespace
     * and the state of WriteBlockBypass. Used for serverStatus.
     */
    MONGO_MOD_FILE_PRIVATE bool isReplicaSetDeletionsBlockingEnabled_forTest() const;

    /**
     * Reports replica set write blocking counters, specifying one counter per blocking reason.
     */
    void appendReplicaSetWritesBlockCounters(BSONObjBuilder& bob) const;

    /**
     * Reports how many operations were rejected by replica set write/deletion blocking.
     */
    void appendReplicaSetWriteBlockRejectionMetrics(BSONObjBuilder& bob) const;

    // The current usage of the following two helpers is extremely short-lived. However,
    // they have been introduced as an early groundwork for SERVER-128193.
    /**
     * Methods to enable/disable blocking new user index builds on this replica set.
     */
    void enableUserIndexBuildBlocking();
    void disableUserIndexBuildBlocking();

    /**
     * Checks that an index build is allowed to start on the specified namespace. Returns
     * ReplicaSetWritesBlocked if user index builds are disallowed, OK otherwise.
     */
    Status checkIfIndexBuildAllowedToStart(OperationContext* opCtx,
                                           const NamespaceString& nss) const;

private:
    struct WriteBlockInfo {
        bool blocked{false};
        ReplicaSetWritesBlockReasonEnum reason{
            ReplicaSetWritesBlockReasonEnum::kInsufficientDiskSpace};
    };
    // Atomic<T> enforces lock-free access at compile time.
    Atomic<WriteBlockInfo> _writeBlockInfo{WriteBlockInfo{}};
    Atomic<bool> _deletionsBlocked{false};
    std::array<AtomicWord<std::uint64_t>, idlEnumCount<ReplicaSetWritesBlockReasonEnum>>
        _replicaSetWritesBlockCounters{};
    mutable AtomicWord<std::uint64_t> _replicaSetWriteBlockRejectedInserts{0};
    mutable AtomicWord<std::uint64_t> _replicaSetWriteBlockRejectedUpdates{0};
    mutable AtomicWord<std::uint64_t> _replicaSetWriteBlockRejectedDeletes{0};
    Atomic<bool> _userIndexBuildsBlocked{false};
};

}  // namespace mongo
