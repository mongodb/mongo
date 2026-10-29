// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#pragma once

#include "mongo/base/status.h"
#include "mongo/db/repl/replication_consistency_markers.h"
#include "mongo/db/repl/replication_recovery.h"
#include "mongo/util/modules.h"

#include <memory>
#include <mutex>

namespace [[MONGO_MOD_PUBLIC]] mongo {

class OperationContext;
class ServiceContext;

namespace repl {

class StorageInterface;

/**
 * This class represents the current replication process state that is used during the replication
 * of operations from the sync source to the current node.
 *
 * For example, the rollback ID, which is persisted to storage, is cached here for the purposes of
 * filling in the metadata for the find/getMore queries used to tail the oplog on the sync source.
 *
 * This class DOES NOT hold any information related to the consensus protocol.
 */
class ReplicationProcess {
    ReplicationProcess(const ReplicationProcess&) = delete;
    ReplicationProcess& operator=(const ReplicationProcess&) = delete;

public:
    constexpr static int kUninitializedRollbackId = -1;

    // Operation Context binding.
    static ReplicationProcess* get(ServiceContext* service);
    static ReplicationProcess* get(ServiceContext& service);
    static ReplicationProcess* get(OperationContext* opCtx);
    static void set(ServiceContext* service, std::unique_ptr<ReplicationProcess> process);

    ReplicationProcess(StorageInterface* storageInterface,
                       std::unique_ptr<ReplicationConsistencyMarkers> consistencyMarkers,
                       std::unique_ptr<ReplicationRecovery> recovery);
    virtual ~ReplicationProcess() = default;

    /**
     * Rollback ID is an increasing counter of how many rollbacks have occurred on this server.
     */
    Status refreshRollbackID(OperationContext* opCtx);
    int getRollbackID() const;
    Status initializeRollbackID(OperationContext* opCtx);
    Status incrementRollbackID(OperationContext* opCtx);

    /**
     * Returns an object used for operating on the documents that maintain replication consistency.
     */
    ReplicationConsistencyMarkers* getConsistencyMarkers();

    /**
     * Returns an object used to recover from the oplog on startup or rollback.
     */
    ReplicationRecovery* getReplicationRecovery();

private:
    // All member variables are labeled with one of the following codes indicating the
    // synchronization rules for accessing them.
    //
    // (R)  Read-only in concurrent operation; no synchronization required.
    // (S)  Self-synchronizing; access in any way from any context.
    // (M)  Reads and writes guarded by _mutex.

    // Guards access to member variables.
    mutable std::mutex _mutex;

    // Used to access the storage layer.
    StorageInterface* const _storageInterface;  // (R)

    // Used for operations on documents that maintain replication consistency.
    std::unique_ptr<ReplicationConsistencyMarkers> _consistencyMarkers;  // (S)

    std::unique_ptr<ReplicationRecovery> _recovery;  // (S)

    // Rollback ID. This is a cached copy of the persisted value in the local.system.rollback.id
    // collection.
    int _rbid;  // (M)
};

}  // namespace repl
}  // namespace mongo
