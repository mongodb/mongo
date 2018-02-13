/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#pragma once

#include <memory>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_consistency_markers.h"
#include "mongo/db/repl/replication_recovery.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

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
    MONGO_DISALLOW_COPYING(ReplicationProcess);

public:
    static const int kUninitializedRollbackId = -1;

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
    mutable stdx::mutex _mutex;

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
