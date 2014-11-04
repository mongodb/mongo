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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/rs_initialsync.h"

#include "mongo/bson/optime.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/client.h"
#include "mongo/db/cloner.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/initial_sync.h"
#include "mongo/db/repl/minvalid.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplogreader.h"
#include "mongo/db/repl/repl_coordinator_global.h"
#include "mongo/db/repl/rslog.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace repl {
namespace {

    bool _initialSyncClone(OperationContext* txn,
                           Cloner& cloner,
                           const std::string& host,
                           const list<string>& dbs,
                           bool dataPass) {

        for( list<string>::const_iterator i = dbs.begin(); i != dbs.end(); i++ ) {
            const string db = *i;
            if( db == "local" ) 
                continue;
            
            if ( dataPass )
                log() << "initial sync cloning db: " << db;
            else
                log() << "initial sync cloning indexes for : " << db;

            string err;
            int errCode;
            CloneOptions options;
            options.fromDB = db;
            options.logForRepl = false;
            options.slaveOk = true;
            options.useReplAuth = true;
            options.snapshot = false;
            options.mayYield = true;
            options.mayBeInterrupted = false;
            options.syncData = dataPass;
            options.syncIndexes = ! dataPass;

            // Make database stable
            Lock::DBLock dbWrite(txn->lockState(), db, MODE_X);

            if (!cloner.go(txn, db, host, options, NULL, err, &errCode)) {
                log() << "initial sync: error while "
                      << (dataPass ? "cloning " : "indexing ") << db
                      << ".  " << (err.empty() ? "" : err + ".  ");
                return false;
            }
        }

        return true;
    }

    /**
     * Replays the sync target's oplog from lastOp to the latest op on the sync target.
     *
     * @param syncer either initial sync (can reclone missing docs) or "normal" sync (no recloning)
     * @param r      the oplog reader
     * @param source the sync target
     * @return if applying the oplog succeeded
     */
    bool _initialSyncApplyOplog( OperationContext* ctx,
                                 repl::SyncTail& syncer,
                                 OplogReader* r) {
        const OpTime startOpTime = getGlobalReplicationCoordinator()->getMyLastOptime();
        BSONObj lastOp;
        try {
            // It may have been a long time since we last used this connection to
            // query the oplog, depending on the size of the databases we needed to clone.
            // A common problem is that TCP keepalives are set too infrequent, and thus
            // our connection here is terminated by a firewall due to inactivity.
            // Solution is to increase the TCP keepalive frequency.
            lastOp = r->getLastOp(rsoplog);
        } catch ( SocketException & ) {
            HostAndPort host = r->getHost();
            log() << "connection lost to " << host.toString() << 
                "; is your tcp keepalive interval set appropriately?";
            if( !r->connect(host) ) {
                error() << "initial sync couldn't connect to " << host.toString();
                throw;
            }
            // retry
            lastOp = r->getLastOp(rsoplog);
        }

        if (lastOp.isEmpty()) {
            error() << "initial sync lastOp is empty";
            sleepsecs(1);
            return false;
        }

        OpTime stopOpTime = lastOp["ts"]._opTime();

        // If we already have what we need then return.
        if (stopOpTime == startOpTime)
            return true;

        verify( !stopOpTime.isNull() );
        verify( stopOpTime > startOpTime );

        // apply till stopOpTime
        try {
            LOG(2) << "Applying oplog entries from " << startOpTime.toStringPretty()
                   << " until " << stopOpTime.toStringPretty();
            syncer.oplogApplication(ctx, stopOpTime);
        }
        catch (const DBException&) {
            log() << "replSet initial sync failed during oplog application phase, and will retry"
                  << rsLog;

            getGlobalReplicationCoordinator()->setMyLastOptime(ctx, OpTime());
            BackgroundSync::get()->setLastAppliedHash(0);

            sleepsecs(5);
            return false;
        }
        
        return true;
    }

    void _tryToApplyOpWithRetry(OperationContext* txn, InitialSync* init, const BSONObj& op) {
        try {
            if (!init->syncApply(txn, op)) {
                bool retry;
                {
                    Lock::GlobalWrite lk(txn->lockState());
                    retry = init->shouldRetry(txn, op);
                }

                if (retry) {
                    // retry
                    if (!init->syncApply(txn, op)) {
                        uasserted(28542,
                                  str::stream() << "During initial sync, failed to apply op: "
                                                << op);
                    }
                }
                // If shouldRetry() returns false, fall through.
                // This can happen if the document that was moved and missed by Cloner
                // subsequently got deleted and no longer exists on the Sync Target at all
            }
        }
        catch (const DBException& e) {
            error() << "exception: " << causedBy(e) << " on: " << op.toString();
            uasserted(28541,
                      str::stream() << "During initial sync, failed to apply op: "
                                    << op);
        }
    }

    /**
     * Do the initial sync for this member.  There are several steps to this process:
     *
     *     0. Add _initialSyncFlag to minValid collection to tell us to restart initial sync if we
     *        crash in the middle of this procedure
     *     1. Record start time.
     *     2. Clone.
     *     3. Set minValid1 to sync target's latest op time.
     *     4. Apply ops from start to minValid1, fetching missing docs as needed.
     *     5. Set minValid2 to sync target's latest op time.
     *     6. Apply ops from minValid1 to minValid2.
     *     7. Build indexes.
     *     8. Set minValid3 to sync target's latest op time.
     *     9. Apply ops from minValid2 to minValid3.
          10. Cleanup minValid collection: remove _initialSyncFlag field, set ts to minValid3 OpTime
     *
     * At that point, initial sync is finished.  Note that the oplog from the sync target is applied
     * three times: step 4, 6, and 8.  4 may involve refetching, 6 should not.  By the end of 6,
     * this member should have consistent data.  8 is "cosmetic," it is only to get this member
     * closer to the latest op time before it can transition out of startup state
     */
    void _initialSync() {
        BackgroundSync* bgsync(BackgroundSync::get());
        InitialSync init(bgsync);
        SyncTail tail(bgsync, multiSyncApply);
        log() << "initial sync pending";

        OplogReader r;
        OpTime now(Milliseconds(curTimeMillis64()).total_seconds(), 0);
        OperationContextImpl txn;

        ReplicationCoordinator* replCoord(getGlobalReplicationCoordinator());

        // We must prime the sync source selector so that it considers all candidates regardless
        // of oplog position, by passing in "now" as the last op fetched time.
        r.connectToSyncSource(&txn, now, replCoord);
        if (r.getHost().empty()) {
            log() << "no valid sync sources found in current replset to do an initial sync";
            sleepsecs(3);
            return;
        }

        init.setHostname(r.getHost().toString());

        BSONObj lastOp = r.getLastOp(rsoplog);
        if ( lastOp.isEmpty() ) {
            log() << "initial sync couldn't read remote oplog";
            sleepsecs(15);
            return;
        }

        if (getGlobalReplicationCoordinator()->getSettings().fastsync) {
            log() << "fastsync: skipping database clone" << rsLog;

            // prime oplog
            _tryToApplyOpWithRetry(&txn, &init, lastOp);
            _logOpObjRS(&txn, lastOp);
            return;
        }

        // Add field to minvalid document to tell us to restart initial sync if we crash
        setInitialSyncFlag(&txn);

        log() << "initial sync drop all databases";
        dropAllDatabasesExceptLocal(&txn);

        log() << "initial sync clone all databases";

        list<string> dbs = r.conn()->getDatabaseNames();

        Cloner cloner;
        if (!_initialSyncClone(&txn, cloner, r.conn()->getServerAddress(), dbs, true)) {
            return;
        }

        log() << "initial sync data copy, starting syncup";

        // prime oplog
        _tryToApplyOpWithRetry(&txn, &init, lastOp);
        _logOpObjRS(&txn, lastOp);

        log() << "oplog sync 1 of 3" << endl;
        if (!_initialSyncApplyOplog(&txn, init, &r)) {
            return;
        }

        // Now we sync to the latest op on the sync target _again_, as we may have recloned ops
        // that were "from the future" compared with minValid. During this second application,
        // nothing should need to be recloned.
        log() << "oplog sync 2 of 3" << endl;
        if (!_initialSyncApplyOplog(&txn, init, &r)) {
            return;
        }
        // data should now be consistent

        log() << "initial sync building indexes";
        if (!_initialSyncClone(&txn, cloner, r.conn()->getServerAddress(), dbs, false)) {
            return;
        }

        log() << "oplog sync 3 of 3" << endl;
        if (!_initialSyncApplyOplog(&txn, tail, &r)) {
            return;
        }
        
        // ---------

        Status status = getGlobalAuthorizationManager()->initialize(&txn);
        if (!status.isOK()) {
            warning() << "Failed to reinitialize auth data after initial sync. " << status;
            return;
        }

        log() << "initial sync finishing up";

        {
            AutoGetDb autodb(&txn, "local", MODE_X);
            OpTime lastOpTimeWritten(getGlobalReplicationCoordinator()->getMyLastOptime());
            log() << "replSet set minValid=" << lastOpTimeWritten << rsLog;

            // Initial sync is now complete.  Flag this by setting minValid to the last thing
            // we synced.
            WriteUnitOfWork wunit(&txn);
            setMinValid(&txn, lastOpTimeWritten);

            // Clear the initial sync flag.
            clearInitialSyncFlag(&txn);
            BackgroundSync::get()->setInitialSyncRequestedFlag(false);
            wunit.commit();
        }

        // If we just cloned & there were no ops applied, we still want the primary to know where
        // we're up to
        bgsync->notify();

        log() << "initial sync done";
    }
} // namespace

    void syncDoInitialSync() {
        static const int maxFailedAttempts = 10;

        {
            OperationContextImpl txn;
            createOplog(&txn);
        }

        int failedAttempts = 0;
        while ( failedAttempts < maxFailedAttempts ) {
            try {
                _initialSync();
                break;
            }
            catch(DBException& e) {
                failedAttempts++;
                mongoutils::str::stream msg;
                error() << "initial sync exception: " << e.toString() << " " << 
                    (maxFailedAttempts - failedAttempts) << " attempts remaining";
                sleepsecs(5);
            }
        }
        fassert( 16233, failedAttempts < maxFailedAttempts);
    }

} // namespace repl
} // namespace mongo
