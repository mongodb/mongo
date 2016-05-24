/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include <iosfwd>
#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/multiapplier.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/service_context.h"

namespace mongo {

class OperationContext;

namespace repl {

struct BatchBoundaries {
    BatchBoundaries(const OpTime s, const OpTime e) : start(s), end(e) {}
    bool operator==(const BatchBoundaries& rhs) const;
    std::string toString() const;
    OpTime start;
    OpTime end;
};

std::ostream& operator<<(std::ostream& stream, const BatchBoundaries& boundaries);

enum class DurableRequirement {
    None,    // Does not require any durability of the write.
    Strong,  // Requires journal or checkpoint write.
};

/**
 * Storage interface used by used by the ReplicationExecutor inside mongod for supporting
 * ReplicationExectutor's ability to take database locks.
 *
 * This interface provides helper functions for maintaining a single document in the
 * local.replset.minvalid collection.
 *
 * When a member reaches its minValid optime it is in a consistent state.  Thus, minValid is
 * set as the last step in initial sync.  At the beginning of initial sync, doingInitialSync
 * is appended onto minValid to indicate that initial sync was started but has not yet
 * completed.
 *
 * The document is also updated during "normal" sync. The optime of the last op in each batch is
 * used to set minValid, along with a "begin" field to demark the start and the fact that a batch
 * is active. When the batch is done the "begin" field is removed to indicate that we are in a
 * consistent state when the batch has been fully applied.
 *
 * Example of all fields:
 * { _id:...,
 *      doingInitialSync: true // initial sync is active
 *      ts:..., t:...   // end-OpTime
 *      begin: {ts:..., t:...} // a batch is currently being applied, and not consistent
 * }
 */

class StorageInterface {
    MONGO_DISALLOW_COPYING(StorageInterface);

public:
    static StorageInterface* get(ServiceContext* service);
    static StorageInterface* get(ServiceContext& service);
    static StorageInterface* get(OperationContext* txn);

    static void set(ServiceContext* service, std::unique_ptr<StorageInterface> storageInterface);

    StorageInterface() = default;
    virtual ~StorageInterface() = default;

    /**
     * Returns true if initial sync was started but has not not completed.
     */
    virtual bool getInitialSyncFlag(OperationContext* txn) const = 0;

    /**
     * Sets the the initial sync flag to record that initial sync has not completed.
     *
     * This operation is durable and waits for durable writes (which will block on
     *journaling/checkpointing).
     */
    virtual void setInitialSyncFlag(OperationContext* txn) = 0;

    /**
     * Clears the the initial sync flag to record that initial sync has completed.
     *
     * This operation is durable and waits for durable writes (which will block on
     *journaling/checkpointing).
     */
    virtual void clearInitialSyncFlag(OperationContext* txn) = 0;

    /**
     * Returns the bounds of the current apply batch, if active. If start is null/missing, and
     * end is equal to the last oplog entry then we are in a consistent state and ready for reads.
     */
    virtual BatchBoundaries getMinValid(OperationContext* txn) const = 0;

    /**
     * The minValid value is the earliest (minimum) Timestamp that must be applied in order to
     * consider the dataset consistent.
     *
     * This is called when a batch finishes.
     *
     * Wait for durable writes (which will block on journaling/checkpointing) when specified.
     *
     */
    virtual void setMinValid(OperationContext* txn,
                             const OpTime& endOpTime,
                             const DurableRequirement durReq) = 0;

    /**
     * The bounds indicate an apply is active and we are not in a consistent state to allow reads
     * or transition from a non-visible state to primary/secondary.
     */
    virtual void setMinValid(OperationContext* txn, const BatchBoundaries& boundaries) = 0;

    /**
     * Writes operations into the replica set oplog at "nss".
     * Used internally by replication secondaries.
     *
     * Returns the optime for the last operation inserted on success.
     */
    virtual StatusWith<OpTime> writeOpsToOplog(OperationContext* txn,
                                               const NamespaceString& nss,
                                               const MultiApplier::Operations& operations) = 0;
};

}  // namespace repl
}  // namespace mongo
