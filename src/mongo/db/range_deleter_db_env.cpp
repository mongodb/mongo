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
#include "mongo/db/clientcursor.h"
#include "mongo/db/cmdline.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/repl/rs.h"
#include "mongo/db/repl/write_concern.h"
#include "mongo/s/d_logic.h"

namespace mongo {

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
    bool RangeDeleterDBEnv::deleteRange(const StringData& ns,
                                        const BSONObj& inclusiveLower,
                                        const BSONObj& exclusiveUpper,
                                        const BSONObj& keyPattern,
                                        bool secondaryThrottle,
                                        std::string* errMsg) {
        const bool initiallyHaveClient = haveClient();

        if (!initiallyHaveClient) {
            Client::initThread("RangeDeleter");
        }

        if (AuthorizationManager::isAuthEnabled()) {
            cc().getAuthorizationSession()->grantInternalAuthorization();
        }

        ShardForceVersionOkModeBlock forceVersion;
        {
            Helpers::RemoveSaver removeSaver("moveChunk", ns.toString(), "post-cleanup");

            // log the opId so the user can use it to cancel the delete using killOp.
            unsigned int opId = cc().curop()->opNum();
            log() << "Deleter starting delete for: " << ns
                  << " from " << inclusiveLower
                  << " -> " << exclusiveUpper
                  << ", with opId: " << opId
                  << endl;

            try {
                long long numDeleted =
                        Helpers::removeRange(KeyRange(ns.toString(),
                                                      inclusiveLower,
                                                      exclusiveUpper,
                                                      keyPattern),
                                             false, /*maxInclusive*/
                                             replSet? secondaryThrottle : false,
                                             cmdLine.moveParanoia ? &removeSaver : NULL,
                                             true, /*fromMigrate*/
                                             true); /*onlyRemoveOrphans*/

                if (numDeleted < 0) {
                    warning() << "collection or index dropped "
                              << "before data could be cleaned" << endl;

                    if (!initiallyHaveClient) {
                        cc().shutdown();
                    }

                    return false;
                }

                log() << "rangeDeleter deleted " << numDeleted
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

        if (replSet) {
            Timer elapsedTime;
            ReplTime lastOpApplied = cc().getLastOp().asDate();
            while (!opReplicatedEnough(lastOpApplied,
                                       BSON("w" << "majority").firstElement())) {
                if (elapsedTime.seconds() >= 3600) {
                    *errMsg = str::stream() << "moveChunk repl sync timed out after "
                                            << elapsedTime.seconds() << " seconds";

                    if (!initiallyHaveClient) {
                        cc().shutdown();
                    }

                    return false;
                }

                sleepsecs(1);
            }

            LOG(elapsedTime.seconds() < 30 ? 1 : 0)
                << "moveChunk repl sync took "
                << elapsedTime.seconds() << " seconds" << endl;
        }

        if (!initiallyHaveClient) {
            cc().shutdown();
        }

        return true;
    }

    void RangeDeleterDBEnv::getCursorIds(const StringData& ns,
                                         std::set<CursorId>* openCursors) {
        ClientCursor::find(ns.toString(), *openCursors);
    }
}
