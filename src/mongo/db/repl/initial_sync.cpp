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

#include "mongo/pch.h"

#include "mongo/db/repl/initial_sync.h"

#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/rs.h"

namespace mongo {
    extern unsigned replSetForceInitialSyncFailure;

namespace replset {
    static AtomicUInt32 replWriterWorkerId;
    void initializeWriterThread() {
        // Only do this once per thread
        if (!ClientBasic::getCurrent()) {
            string threadName = str::stream() << "repl writer worker "
                                              << replWriterWorkerId.addAndFetch(1);
            Client::initThread( threadName.c_str() );
            // allow us to get through the magic barrier
            Lock::ParallelBatchWriterMode::iAmABatchParticipant();
            replLocalAuth();
        }
    }

    // This free function is used by the writer threads to apply each op
    void multiSyncApply(const std::vector<BSONObj>& ops, SyncTail* st) {
        initializeWriterThread();

        // convert update operations only for 2.2.1 or greater, because we need guaranteed
        // idempotent operations for this to work.  See SERVER-6825
        bool convertUpdatesToUpserts = theReplSet->oplogVersion > 1 ? true : false;

        for (std::vector<BSONObj>::const_iterator it = ops.begin();
             it != ops.end();
             ++it) {
            try {
                if (!st->syncApply(*it, convertUpdatesToUpserts)) {
                    fassertFailedNoTrace(16359);
                }
            } catch (const DBException& e) {
                error() << "writer worker caught exception: " << causedBy(e)
                        << " on: " << it->toString() << endl;
                fassertFailedNoTrace(16360);
            }
        }
    }

    // This free function is used by the initial sync writer threads to apply each op
    void multiInitialSyncApply(const std::vector<BSONObj>& ops, SyncTail* st) {
        initializeWriterThread();
        for (std::vector<BSONObj>::const_iterator it = ops.begin();
             it != ops.end();
             ++it) {
            try {
                if (!st->syncApply(*it)) {
                    bool status;
                    {
                        Lock::GlobalWrite lk;
                        status = st->shouldRetry(*it);
                    }
                    if (status) {
                        // retry
                        if (!st->syncApply(*it)) {
                            fassertFailedNoTrace(15915);
                        }
                    }
                    // If shouldRetry() returns false, fall through.
                    // This can happen if the document that was moved and missed by Cloner
                    // subsequently got deleted and no longer exists on the Sync Target at all
                }
            }
            catch (const DBException& e) {
                error() << "exception: " << causedBy(e) << " on: " << it->toString() << endl;
                fassertFailedNoTrace(16361);
            }
        }
    }


    InitialSync::InitialSync(BackgroundSyncInterface *q) : 
        SyncTail(q) {}

    InitialSync::~InitialSync() {}

    /* initial oplog application, during initial sync, after cloning.
    */
    BSONObj InitialSync::oplogApplication(const BSONObj& applyGTEObj, const BSONObj& minValidObj) {
        if (replSetForceInitialSyncFailure > 0) {
            log() << "replSet test code invoked, forced InitialSync failure: "
                  << replSetForceInitialSyncFailure << rsLog;
            replSetForceInitialSyncFailure--;
            throw DBException("forced error",0);
        }

        // create the initial oplog entry
        syncApply(applyGTEObj);
        _logOpObjRS(applyGTEObj);

        return oplogApplySegment(applyGTEObj, minValidObj, multiInitialSyncApply);
    }

} // namespace replset
} // namespace mongo
