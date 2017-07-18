/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/base/status.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/optime.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

/**
 * This rollback algorithm requires featureCompatibilityVersion 3.6.
 *
 * Rollback Overview:
 *
 * Rollback occurs when a node's oplog diverges from its sync source's oplog and needs to regain
 * consistency with the sync source's oplog.
 *
 * R and S are defined below to represent two nodes involved in rollback.
 *
 *     R = The node whose oplog has diverged from its sync source and is rolling back.
 *     S = The sync source of node R.
 *
 * The rollback algorithm is designed to keep S's data and to make node R consistent with node S.
 * One could argue here that keeping R's data has some merits, however, in most
 * cases S will have significantly more data.  Also note that S may have a proper subset of R's
 * stream if there were no subsequent writes. Our goal is to get R back in sync with S.
 *
 * A visualization of what happens in the oplogs of the sync source and node that is rolling back
 * is shown below. On the left side of each example are the oplog entries of the nodes before
 * rollback occurs and on the right are the oplog entries of the node after rollback occurs.
 * During rollback only the oplog entries of R change.
 *
 * #1: Status of R after operations e, f, and g are rolled back to the common point [d].
 * Since there were no other writes to node S after [d], we do not need to apply
 * more writes to node R after rolling back.
 *
 *     R : a b c d e f g                -> a b c d
 *     S : a b c d
 *
 * #2: In this case, we first roll back to [d], and since S has written q to z oplog entries,
 * we need to replay these oplog operations onto R after it has rolled back to the common
 * point.
 *
 *     R : a b c d e f g                -> a b c d q r s t u v w x z
 *     S : a b c d q r s t u v w x z
 *
 * Rollback Algorithm:
 *
 * We will continue to use the notation of R as the node whose oplog is inconsistent with
 * its sync source and S as the sync source of R. We will also represent the common point
 * as point C.
 *
 * 1.   Increment rollback ID of node R.
 * 2.   Find the most recent common oplog entry, which we will say is point C. In the above
 *      example, the common point was oplog entry 'd'.
 * 3.   Undo all the oplog entries that occurred after point C on the node R.
 *      a.    Consider how to revert the oplog entries (i.e. for a create collection, drops the
 *            collection) and place that information into a FixUpInfo struct.
 *      b.    Cancel out unnecessary operations (i.e. If dropping a collection, there is no need
 *            to do dropIndex if the index is within the collection that will eventually be
 *            dropped).
 *      c.    Undo all operations done on node R until the point. We attempt to revert all data
 *            and metadata until point C. However, if we need to refetch data from the sync
 *            source, the data on node R will not be completely consistent with what it was
 *            previously at point C, as some of the data may have been modified by the sync
 *            source after the common point.
 *            i.    Refetch any documents from node S that are needed for the
 *                  rollback.
 *           ii.    Find minValid, which is the last OpTime of node S.
 *                  i.e. the last oplog entry of node S at the time that rollback occurs.
 *          iii.    Resync collection data and metadata.
 *           iv.    Update minValid if necessary, as more fetching may have occurred from
 *                  the sync source requiring that minValid is updated to an even later
 *                  point.
 *            v.    Drop all collections that were created after point C.
 *           vi.    Drop all indexes that were created after point C.
 *          vii.    Delete, update and insert necessary documents that were modified after
 *                  point C.
 *         viii.    Truncate the oplog to point C.
 * 4.   After rolling back to point C, node R transitions from ROLLBACK to RECOVERING mode.
 *
 * Steps 5 and 6 occur in ordinary replication code and are not done in this file.
 *
 * 5.   Retrieve the oplog entries from node S until reaching the minValid oplog entry.
 *      a.    Fetch the oplog entries from node S.
 *      b.    Apply the oplog entries of node S to node R starting from point C up until
 *            the minValid
 * 6.   Transition node R from RECOVERING to SECONDARY state.
 */

namespace mongo {

class DBClientConnection;
class OperationContext;

namespace repl {

class OplogInterface;
class ReplicationCoordinator;
class ReplicationProcess;
class RollbackSource;

/**
 * Entry point to rollback process.
 * Set state to ROLLBACK while we are in this function. This prevents serving reads, even from
 * the oplog. This can fail if we are elected PRIMARY, in which case we better not do any
 * rolling back. If we successfully enter ROLLBACK, we will only exit this function fatally or
 * after transition to RECOVERING.
 *
 * 'sleepSecsFn' is an optional testing-only argument for overriding mongo::sleepsecs().
 */

void rollback(OperationContext* opCtx,
              const OplogInterface& localOplog,
              const RollbackSource& rollbackSource,
              int requiredRBID,
              ReplicationCoordinator* replCoord,
              ReplicationProcess* replicationProcess,
              stdx::function<void(int)> sleepSecsFn = [](int secs) { sleepsecs(secs); });

/**
 * Initiates the rollback process after transition to ROLLBACK.
 * This function assumes the preconditions for undertaking rollback have already been met;
 * we have ops in our oplog that our sync source does not have, and we are not currently
 * PRIMARY.
 *
 * This function can throw exceptions on failures.
 * This function runs a command on the sync source to detect if the sync source rolls back
 * while our rollback is in progress.
 *
 * @param opCtx: Used to read and write from this node's databases.
 * @param localOplog: reads the oplog on this server.
 * @param rollbackSource: Interface for sync source. Provides the oplog and
 *            supports fetching documents and copying collections.
 * @param requiredRBID: Rollback ID we are required to have throughout rollback.
 * @param replCoord: Used to track the rollback ID and to change the follower state.
 * @param replicationProcess: Used to update minValid.
 *
 * If requiredRBID is supplied, we error if the upstream node has a different RBID (i.e. it rolled
 * back) after fetching any information from it.
 *
 * Failures: If a Status with code UnrecoverableRollbackError is returned, the caller must exit
 * fatally. All other errors should be considered recoverable regardless of whether reported as a
 * status or exception.
 */
Status syncRollback(OperationContext* opCtx,
                    const OplogInterface& localOplog,
                    const RollbackSource& rollbackSource,
                    int requiredRBID,
                    ReplicationCoordinator* replCoord,
                    ReplicationProcess* replicationProcess);


/*
Rollback function flowchart:

1.    rollback() called.
      a.    syncRollback() called by rollback().
            i.    _syncRollback() called by syncRollback().
                  I.    syncRollbackLocalOperations() called by _syncRollback().
                        A.    processOperationFixUp called by syncRollbackLocalOperations().
                              1.    updateFixUpInfoFromLocalOplogEntry called by
                                    processOperationFixUp().
                 II.    removeRedundantOperations() called by _syncRollback().
                III.    syncFixUp() called by _syncRollback().
                        1.    Retrieves documents to refetch.
                        2.    Checks the rollback ID and updates minValid.
                        3.    Resyncs collection data and metadata.
                        4.    Checks the rollbackID and updates minValid.
                        5.    Drops collections.
                        6.    Drops indexes.
                        7.    Deletes, updates and inserts individual oplogs.
                        8.    Truncates the oplog.
                 IV.    Returns back to syncRollback().
           ii.   Returns back to rollback().
      b.   Rollback ends.
*/


/**
 * This namespace contains internal details of the rollback system. It is only exposed in a header
 * for unit testing. Nothing here should be used outside of rs_rollback.cpp or its unit test.
 */
namespace rollback_internal {

struct DocID {
    BSONObj ownedObj;
    const char* ns;
    BSONElement _id;
    bool operator<(const DocID& other) const;
    bool operator==(const DocID& other) const;

    static DocID minFor(const char* ns) {
        auto obj = BSON("" << MINKEY);
        return {obj, ns, obj.firstElement()};
    }

    static DocID maxFor(const char* ns) {
        auto obj = BSON("" << MAXKEY);
        return {obj, ns, obj.firstElement()};
    }
};

struct FixUpInfo {
    // Note this is a set -- if there are many $inc's on a single document we need to roll back,
    // we only need to refetch it once.
    std::set<DocID> docsToRefetch;

    // Namespaces of collections that need to be dropped or resynced from the sync source.
    std::set<std::string> collectionsToResyncData;

    // UUIDs of collections that need to be dropped.
    stdx::unordered_set<UUID, UUID::Hash> collectionsToDrop;

    // Key is the UUID of the collection. Value is the set of index names to drop for each
    // collection.
    stdx::unordered_map<UUID, std::set<std::string>, UUID::Hash> indexesToDrop;

    // Key is the UUID of the collection. Value is a map from indexName to indexSpec for the index.
    stdx::unordered_map<UUID, std::map<std::string, BSONObj>, UUID::Hash> indexesToCreate;

    // UUIDs of collections that need to have their metadata resynced from the sync source.
    stdx::unordered_set<UUID, UUID::Hash> collectionsToResyncMetadata;

    // When collections are dropped, they are added to a list of drop-pending collections. We keep
    // the OpTime and the namespace of the collection because the DropPendingCollectionReaper
    // does not store the original name or UUID of the collection.
    stdx::unordered_map<UUID, std::pair<OpTime, NamespaceString>, UUID::Hash>
        collectionsToRollBackPendingDrop;

    // True if rollback requires re-fetching documents in the session transaction table. If true,
    // after rollback the in-memory transaction table is cleared.
    bool refetchTransactionDocs = false;

    OpTime commonPoint;
    RecordId commonPointOurDiskloc;

    /**
     * Remote server's current rollback id. Keeping track of this
     * allows us to determine if the sync source has rolled back, in which case
     * we can terminate the rollback of the local node, as we cannot
     * roll back against a sync source that is also rolled back.
     */
    int rbid;

    /**
     * Removes all documents in the docsToRefetch set that are in
     * the collection passed into the function.
     */
    void removeAllDocsToRefetchFor(const std::string& collection);

    /**
     * Removes any redundant operations that may have happened during
     * the period of time that the rolling back node was out of sync
     * with its sync source. For example, if a collection is dropped, there is
     * no need to also drop the indexes that are part of the collection. This
     * function removes any operations that were recorded that are unnecessary
     * because the collection that the operation is part of is either going
     * to be dropped, or fully resynced.
     */
    void removeRedundantOperations();

    /**
     * Removes any redundant index commands. An example is if we create
     * an index with name "a_1" and then later proceed to drop that index.
     * We return true if a redundant index command was found and false
     * if it was not.
     */
    bool removeRedundantIndexCommands(UUID uuid, std::string indexName);
};

// Indicates that rollback cannot complete and the server must abort.
class RSFatalException : public std::exception {
public:
    RSFatalException(std::string m = "replica set fatal exception") : msg(m) {}
    virtual const char* what() const throw() {
        return msg.c_str();
    }

private:
    std::string msg;
};

/**
 * This function goes through a single oplog document of the node and records the necessary
 * information in order to undo the given oplog entry. The data is placed into a FixUpInfo
 * struct that holds all the necessary information to undo all of the oplog entries of the
 * rolling back node from after the common point. "ourObj" is the oplog document that needs
 * to be reverted.
 */
Status updateFixUpInfoFromLocalOplogEntry(FixUpInfo& fixUpInfo, const BSONObj& ourObj);
}  // namespace rollback_internal
}  // namespace repl
}  // namespace mongo
