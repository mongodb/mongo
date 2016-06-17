/**
*    Copyright (C) 2008 10gen Inc.
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

#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/stdx/functional.h"

namespace mongo {
class Collection;
class Database;
class NamespaceString;
class OperationContext;

namespace repl {
class ReplSettings;

/**
 * Truncates the oplog after, and including, the "truncateTimestamp" entry.
 */
void truncateOplogTo(OperationContext* txn, Timestamp truncateTimestamp);

/**
 * Create a new capped collection for the oplog if it doesn't yet exist.
 * If the collection already exists (and isReplSet is false),
 * set the 'last' Timestamp from the last entry of the oplog collection (side effect!)
 */
void createOplog(OperationContext* txn, const std::string& oplogCollectionName, bool isReplSet);

/*
 * Shortcut for above function using oplogCollectionName = _oplogCollectionName,
 * and replEnabled = replCoord::isReplSet();
 */
void createOplog(OperationContext* txn);

extern std::string rsOplogName;
extern std::string masterSlaveOplogName;

extern int OPLOG_VERSION;

/* Log operation(s) to the local oplog
 *
 * @param opstr
 *  "i" insert
 *  "u" update
 *  "d" delete
 *  "c" db cmd
 *  "n" no-op
 *  "db" declares presence of a database (ns is set to the db name + '.')
 */

void logOps(OperationContext* txn,
            const char* opstr,
            const NamespaceString& nss,
            std::vector<BSONObj>::const_iterator begin,
            std::vector<BSONObj>::const_iterator end,
            bool fromMigrate);

/* For 'u' records, 'obj' captures the mutation made to the object but not
 * the object itself. 'o2' captures the the criteria for the object that will be modified.
 */
void logOp(OperationContext* txn,
           const char* opstr,
           const char* ns,
           const BSONObj& obj,
           const BSONObj* o2,
           bool fromMigrate);

// Flush out the cached pointers to the local database and oplog.
// Used by the closeDatabase command to ensure we don't cache closed things.
void oplogCheckCloseDatabase(OperationContext* txn, Database* db);

using IncrementOpsAppliedStatsFn = stdx::function<void()>;
/**
 * Take a non-command op and apply it locally
 * Used for applying from an oplog
 * @param convertUpdateToUpsert convert some updates to upserts for idempotency reasons
 * @param incrementOpsAppliedStats is called whenever an op is applied.
 * Returns failure status if the op was an update that could not be applied.
 */
Status applyOperation_inlock(OperationContext* txn,
                             Database* db,
                             const BSONObj& op,
                             bool convertUpdateToUpsert = false,
                             IncrementOpsAppliedStatsFn incrementOpsAppliedStats = {});

/**
 * Take a command op and apply it locally
 * Used for applying from an oplog
 * Returns failure status if the op that could not be applied.
 */
Status applyCommand_inlock(OperationContext* txn, const BSONObj& op);

/**
 * Initializes the global Timestamp with the value from the timestamp of the last oplog entry.
 */
void initTimestampFromOplog(OperationContext* txn, const std::string& oplogNS);

/**
 * Sets the global Timestamp to be 'newTime'.
 */
void setNewTimestamp(const Timestamp& newTime);

/**
 * Detects the current replication mode and sets the "_oplogCollectionName" accordingly.
 */
void setOplogCollectionName();

/**
 * Signal any waiting AwaitData queries on the oplog that there is new data or metadata available.
 */
void signalOplogWaiters();

/**
 * Check that the oplog is capped, and abort the process if it is not.
 */
void checkForCappedOplog(OperationContext* txn);

}  // namespace repl
}  // namespace mongo
