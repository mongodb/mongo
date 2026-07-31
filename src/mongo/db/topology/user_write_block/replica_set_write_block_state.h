// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/topology/user_write_block/replica_set_writes_block_reason_gen.h"
#include "mongo/platform/atomic.h"
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
enum class [[MONGO_MOD_NEEDS_REPLACEMENT]] ReplicaSetWriteBlockRejectedWriteOp { kInsert, kUpdate };

// TODO (SERVER-125476): Change the class modularity to PRIVATE
class [[MONGO_MOD_NEEDS_REPLACEMENT]] ReplicaSetWriteBlockState {
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
     * Checks that an incoming chunk migration is allowed to start on this replica set. Throws
     * ReplicaSetWritesBlocked if incoming migrations are disallowed.
     */
    void checkIfIncomingMigrationAllowedToStart(OperationContext* opCtx) const;

    /**
     * Returns whether replica set deletions blocking is enabled, disregarding a specific namespace
     * and the state of WriteBlockBypass. Used for serverStatus.
     */
    [[MONGO_MOD_FILE_PRIVATE]] bool isReplicaSetDeletionsBlockingEnabled_forTest() const;

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

    /**
     * Checks that an incoming resharding operation is allowed to start on this replica set as a
     * recipient. Returns ReplicaSetWritesBlocked if incoming resharding is disallowed, OK
     * otherwise.
     */
    Status checkIfIncomingReshardingAllowedToStart(OperationContext* opCtx) const;

private:
    struct WriteBlockInfo {
        bool blocked{false};
        ReplicaSetWritesBlockReasonEnum reason{
            ReplicaSetWritesBlockReasonEnum::kInsufficientDiskSpace};
    };
    // Atomic<T> enforces lock-free access at compile time.
    Atomic<WriteBlockInfo> _writeBlockInfo{WriteBlockInfo{}};
    Atomic<bool> _deletionsBlocked{false};
    std::array<Atomic<std::uint64_t>, idlEnumCount<ReplicaSetWritesBlockReasonEnum>>
        _replicaSetWritesBlockCounters{};
    mutable Atomic<std::uint64_t> _replicaSetWritesBlockRejectedInserts{0};
    mutable Atomic<std::uint64_t> _replicaSetWritesBlockRejectedUpdates{0};
    mutable Atomic<std::uint64_t> _replicaSetWritesBlockRejectedDeletes{0};
    Atomic<bool> _userIndexBuildsBlocked{false};
};

}  // namespace mongo
