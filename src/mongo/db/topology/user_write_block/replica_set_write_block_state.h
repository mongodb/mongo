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

#include <boost/optional.hpp>

namespace mongo {

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
    boost::optional<ReplicaSetWritesBlockReasonEnum> getReplicaSetWriteBlockingReason(
        OperationContext* opCtx) const {
        const auto info = _writeBlockInfo.load();
        if (!info.blocked) {
            return boost::none;
        }
        return info.reason;
    }

    /**
     * Checks that replica set writes are allowed on the specified namespace. Callers must hold
     * the GlobalLock in any mode. Throws UserWritesBlocked if user writes are disallowed.
     */
    void checkReplicaSetWritesAllowed(OperationContext* opCtx, const NamespaceString& nss) const;

    /**
     * Returns whether replica set write blocking is enabled, disregarding a specific namespace
     * and the state of WriteBlockBypass. Used for serverStatus.
     */
    bool isReplicaSetWriteBlockingEnabled() const;

    /**
     * Methods to control the replica set deletions blocking state.
     */
    void enableReplicaSetDeletionsBlocking();
    void disableReplicaSetDeletionsBlocking();

    /**
     * Checks that replica set deletions are allowed on the specified namespace. Throws
     * UserWritesBlocked if deletions are disallowed.
     */
    void checkReplicaSetDeletionsAllowed(OperationContext* opCtx, const NamespaceString& nss) const;

    /**
     * Returns whether replica set deletions blocking is enabled, disregarding a specific namespace
     * and the state of WriteBlockBypass. Used for serverStatus.
     */
    MONGO_MOD_FILE_PRIVATE bool isReplicaSetDeletionsBlockingEnabled_forTest() const;

private:
    struct WriteBlockInfo {
        bool blocked{false};
        ReplicaSetWritesBlockReasonEnum reason{
            ReplicaSetWritesBlockReasonEnum::kInsufficientDiskSpace};
    };
    // Atomic<T> enforces lock-free access at compile time.
    Atomic<WriteBlockInfo> _writeBlockInfo{WriteBlockInfo{}};
    Atomic<bool> _deletionsBlocked{false};
};

}  // namespace mongo
