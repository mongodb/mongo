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

#include "mongo/db/range_deleter_db_env.h"

#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/repl/repl_coordinator_global.h"
#include "mongo/db/repl/rs.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/s/d_logic.h"

namespace mongo {

    void RangeDeleterDBEnv::initThread() {
        if ( currentClient.get() == NULL )
            Client::initThread( "RangeDeleter" );
    }

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
        const string ns(taskDetails.ns);
        const BSONObj inclusiveLower(taskDetails.min);
        const BSONObj exclusiveUpper(taskDetails.max);
        const BSONObj keyPattern(taskDetails.shardKeyPattern);
        const bool secondaryThrottle(taskDetails.secondaryThrottle);

        const bool initiallyHaveClient = haveClient();

        if (!initiallyHaveClient) {
            Client::initThread("RangeDeleter");
        }

        *deletedDocs = 0;
        ShardForceVersionOkModeBlock forceVersion;
        {
            Helpers::RemoveSaver removeSaver("moveChunk", ns, "post-cleanup");

            // log the opId so the user can use it to cancel the delete using killOp.
            unsigned int opId = txn->getCurOp()->opNum();
            log() << "Deleter starting delete for: " << ns
                  << " from " << inclusiveLower
                  << " -> " << exclusiveUpper
                  << ", with opId: " << opId
                  << endl;

            try {
                bool throttle = repl::getGlobalReplicationCoordinator()->getReplicationMode() ==
                        repl::ReplicationCoordinator::modeReplSet ? secondaryThrottle : false;
                *deletedDocs =
                        Helpers::removeRange(txn,
                                             KeyRange(ns,
                                                      inclusiveLower,
                                                      exclusiveUpper,
                                                      keyPattern),
                                             false, /*maxInclusive*/
                                             throttle,
                                             serverGlobalParams.moveParanoia ? &removeSaver : NULL,
                                             true, /*fromMigrate*/
                                             true); /*onlyRemoveOrphans*/

                if (*deletedDocs < 0) {
                    *errMsg = "collection or index dropped before data could be cleaned";
                    warning() << *errMsg << endl;

                    if (!initiallyHaveClient) {
                        cc().shutdown();
                    }

                    return false;
                }

                log() << "rangeDeleter deleted " << *deletedDocs
                      << " documents for " << ns
                      << " from " << inclusiveLower
                      << " -> " << exclusiveUpper
                      << endl;
            }
            catch (const DBException& ex) {
                *errMsg = str::stream() << "Error encountered while deleting range: "
                                        << "ns" << ns
                                        << " from " << inclusiveLower
                                        << " -> " << exclusiveUpper
                                        << ", cause by:" << causedBy(ex);

                if (!initiallyHaveClient) {
                    cc().shutdown();
                }

                return false;
            }
        }

        if (!initiallyHaveClient) {
            cc().shutdown();
        }

        return true;
    }

    void RangeDeleterDBEnv::getCursorIds(OperationContext* txn,
                                         const StringData& ns,
                                         std::set<CursorId>* openCursors) {
        Client::ReadContext ctx(txn, ns.toString());
        Collection* collection = ctx.ctx().db()->getCollection( txn, ns );
        if ( !collection )
            return;

        collection->cursorCache()->getCursorIds( openCursors );
    }
}
