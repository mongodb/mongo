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
#include "mongo/db/record_id.h"
#include "mongo/db/repl/optime.h"

namespace mongo {

class DBClientConnection;
class NamespaceString;
class OperationContext;

namespace repl {

class OplogInterface;
class ReplicationCoordinator;
class RollbackSource;

/**
 * Initiates the rollback process.
 * This function assumes the preconditions for undertaking rollback have already been met;
 * we have ops in our oplog that our sync source does not have, and we are not currently
 * PRIMARY.
 * The rollback procedure is:
 * - find the common point between this node and its sync source
 * - undo operations by fetching all documents affected, then replaying
 *   the sync source's oplog until we reach the time in the oplog when we fetched the last
 *   document.
 * This function can throw exceptions on failures.
 * This function runs a command on the sync source to detect if the sync source rolls back
 * while our rollback is in progress.
 *
 * @param txn Used to read and write from this node's databases
 * @param localOplog reads the oplog on this server.
 * @param rollbackSource interface for sync source:
 *            provides oplog; and
 *            supports fetching documents and copying collections.
 * @param replCoord Used to track the rollback ID and to change the follower state
 *
 * If requiredRBID is supplied, we error if the upstream node has a different RBID (ie it rolled
 * back) after fetching any information from it.
 *
 * Failures: If a Status with code UnrecoverableRollbackError is returned, the caller must exit
 * fatally. All other errors should be considered recoverable regardless of whether reported as a
 * status or exception.
 */
Status syncRollback(OperationContext* txn,
                    const OplogInterface& localOplog,
                    const RollbackSource& rollbackSource,
                    boost::optional<int> requiredRBID,
                    ReplicationCoordinator* replCoord);

/**
 * This namespace contains internal details of the rollback system. It is only exposed in a header
 * for unittesting. Nothing here should be used outside of rs_rollback.cpp or its unittest.
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
    // note this is a set -- if there are many $inc's on a single document we need to rollback,
    // we only need to refetch it once.
    std::set<DocID> docsToRefetch;

    // Key is collection namespace. Value is name of index to drop.
    std::multimap<std::string, std::string> indexesToDrop;

    std::set<std::string> collectionsToDrop;
    std::set<std::string> collectionsToResyncData;
    std::set<std::string> collectionsToResyncMetadata;

    OpTime commonPoint;
    RecordId commonPointOurDiskloc;

    int rbid;  // remote server's current rollback sequence #

    void removeAllDocsToRefetchFor(const std::string& collection);
    void removeRedundantOperations();
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

Status updateFixUpInfoFromLocalOplogEntry(FixUpInfo& fixUpInfo, const BSONObj& ourObj);
}  // namespace rollback_internal
}  // namespace repl
}  // namespace mongo
