/**
 *    Copyright (C) 2013 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/range_deleter_db_env.h"

#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/util/log.h"

namespace mongo {

using std::string;

/**
 * Outline of the delete process:
 * 1. Initialize the client for this thread if there is no client. This is for the worker
 *    threads that are attached to any of the threads servicing client requests.
 * 2. Grant this thread authorization to perform deletes.
 * 3. Temporarily enable mode to bypass shard version checks. TODO: Replace this hack.
 * 4. Setup callback to save deletes to moveChunk directory (only if moveParanoia is true).
 * 5. Delete range.
 * 6. Wait until the majority of the secondaries catch up.
 */
bool RangeDeleterDBEnv::deleteRange(OperationContext* txn,
                                    const RangeDeleteEntry& taskDetails,
                                    long long int* deletedDocs,
                                    std::string* errMsg) {
    const string ns(taskDetails.options.range.ns);
    const BSONObj inclusiveLower(taskDetails.options.range.minKey);
    const BSONObj exclusiveUpper(taskDetails.options.range.maxKey);
    const BSONObj keyPattern(taskDetails.options.range.keyPattern);
    const WriteConcernOptions writeConcern(taskDetails.options.writeConcern);
    const bool fromMigrate = taskDetails.options.fromMigrate;
    const bool onlyRemoveOrphans = taskDetails.options.onlyRemoveOrphanedDocs;

    Client::initThreadIfNotAlready("RangeDeleter");

    *deletedDocs = 0;
    OperationShardingState::IgnoreVersioningBlock forceVersion(txn, NamespaceString(ns));

    Helpers::RemoveSaver removeSaver("moveChunk", ns, taskDetails.options.removeSaverReason);
    Helpers::RemoveSaver* removeSaverPtr = NULL;
    if (serverGlobalParams.moveParanoia && !taskDetails.options.removeSaverReason.empty()) {
        removeSaverPtr = &removeSaver;
    }

    // log the opId so the user can use it to cancel the delete using killOp.
    unsigned int opId = txn->getOpID();
    log() << "Deleter starting delete for: " << ns << " from " << inclusiveLower << " -> "
          << exclusiveUpper << ", with opId: " << opId;

    try {
        *deletedDocs =
            Helpers::removeRange(txn,
                                 KeyRange(ns, inclusiveLower, exclusiveUpper, keyPattern),
                                 false, /*maxInclusive*/
                                 writeConcern,
                                 removeSaverPtr,
                                 fromMigrate,
                                 onlyRemoveOrphans);

        if (*deletedDocs < 0) {
            *errMsg = "collection or index dropped before data could be cleaned";
            warning() << *errMsg;

            return false;
        }

        log() << "rangeDeleter deleted " << *deletedDocs << " documents for " << ns << " from "
              << inclusiveLower << " -> " << exclusiveUpper;
    } catch (const DBException& ex) {
        *errMsg = str::stream() << "Error encountered while deleting range: "
                                << "ns" << ns << " from " << inclusiveLower << " -> "
                                << exclusiveUpper << ", cause by:" << causedBy(ex);

        return false;
    }

    return true;
}

void RangeDeleterDBEnv::getCursorIds(OperationContext* txn,
                                     StringData ns,
                                     std::set<CursorId>* openCursors) {
    AutoGetCollection autoColl(txn, NamespaceString(ns), MODE_IS);
    if (!autoColl.getCollection())
        return;

    autoColl.getCollection()->getCursorManager()->getCursorIds(openCursors);
}

}  // namespace mongo
