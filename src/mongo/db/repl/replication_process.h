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
    /**
     * Contains at most one document representing the progress of the current rollback process.
     *
     * Schema:
     *     {_id: "rollbackProgress", applyUntil: <optime>}
     *
     * '_id' is always "rollbackProgress".
     *
     * 'applyUntil' contains the optime of the last oplog entry from the sync source that we need to
     * apply in order to complete rollback successfully.
     */
    static const NamespaceString kRollbackProgressNamespace;

    // Operation Context binding.
    static ReplicationProcess* get(ServiceContext* service);
    static ReplicationProcess* get(ServiceContext& service);
    static ReplicationProcess* get(OperationContext* opCtx);
    static void set(ServiceContext* service, std::unique_ptr<ReplicationProcess> storageInterface);

    // Constructor and Destructor.
    explicit ReplicationProcess(StorageInterface* storageInterface);
    virtual ~ReplicationProcess() = default;

    /**
     * Rollback ID is an increasing counter of how many rollbacks have occurred on this server.
     */
    StatusWith<int> getRollbackID(OperationContext* opCtx);
    Status initializeRollbackID(OperationContext* opCtx);
    Status incrementRollbackID(OperationContext* opCtx);

    /**
     * Rollback progress is set after we have retrieved all the information from the sync source
     * that we need to complete rollback without further communication with the sync source.
     * Rollback progress is cleared when rollback has completed successfully. This information is
     * stored in the 'kRollbackProgressNamespace' collection.
     * If the collection is not empty, it will hold the optime of the oplog entry we pulled down
     * from the sync source into the local.system.rollback.oplog collection that we need to apply
     * through from that collection. It is safe to exit rollback once we have applied this optime.
     *
     * If the collection is not present, we return NamespaceNotFound.
     * If the document is not present, we return NoSuchKey.
     *
     * This function is used at replication startup to check if a previously interrupted rollback
     * process has occurred and that the rollback process can be resumed without contacting any
     * sync source.
     * An error status returned by this function indicates that we did not detect any interrupted
     * rollback and that we can continue with normal replication startup.
     */
    StatusWith<OpTime> getRollbackProgress(OperationContext* opCtx);

    /**
     * Upon success, a document representing the current rollback progress will be present in the
     * 'kRollbackProgressNamespace' collection. This document will contain the optime that this
     * node will have to reach in order to consider rollback complete.
     *
     * If the 'kRollbackProgressNamespace' collection is not present when this function is called,
     * this function will create it before inserting the document.
     */
    Status setRollbackProgress(OperationContext* opCtx, const OpTime& applyUntil);

    /**
     * Removes the rollback progress document from the 'kRollbackProgressNamespace' collection.
     *
     * If the collection is not found, this function will return a successful Status because there's
     * nothing further to do.
     */
    Status clearRollbackProgress(OperationContext* opCtx);

private:
    // All member variables are labeled with one of the following codes indicating the
    // synchronization rules for accessing them.
    //
    // (R)  Read-only in concurrent operation; no synchronization required.
    // (S)  Self-synchronizing; access in any way from any context.
    // (M)  Reads and writes guarded by _mutex.

    // Guards access to member variables.
    stdx::mutex _mutex;

    // Used to access the storage layer.
    StorageInterface* const _storageInterface;  // (R)

    // Rollback ID. This is a cached copy of the persisted value in the local.system.rollback.id
    // collection.
    int _rbid;  // (M)
};

}  // namespace repl
}  // namespace mongo
