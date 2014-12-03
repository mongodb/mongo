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
#include "mongo/db/catalog/collection.h"
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
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace repl {
namespace {

    /**
     * Truncates the oplog (removes any documents) and resets internal variables that were
     * originally initialized or affected by using values from the oplog at startup time.  These 
     * include the last applied optime, the last fetched optime, and the sync source blacklist.
     * Also resets the bgsync thread so that it reconnects its sync source after the oplog has been
     * truncated.
     */
    void truncateAndResetOplog(OperationContext* txn, 
                               ReplicationCoordinator* replCoord,
                               BackgroundSync* bgsync) {
        Client::WriteContext ctx(txn, rsoplog);
        // Note: the following order is important.
        // The bgsync thread uses an empty optime as a sentinel to know to wait
        // for initial sync; thus, we must
        // ensure the lastAppliedOptime is empty before restarting the bgsync thread
        // via stop().
        // We must clear the sync source blacklist after calling stop()
        // because the bgsync thread, while running, may update the blacklist.
        replCoord->setMyLastOptime(OpTime());
        bgsync->stop();
        replCoord->clearSyncSourceBlacklist();

        // Truncate the oplog in case there was a prior initial sync that failed.
        Collection* collection = ctx.getCollection();
        fassert(28565, collection);
        WriteUnitOfWork wunit(txn);
        Status status = collection->truncate(txn);
        fassert(28564, status);
        wunit.commit();
    }

    bool _initialSyncClone(OperationContext* txn,
                           Cloner& cloner,
                           const std::string& host,
                           const list<string>& dbs,
                           bool dataPass) {

        for( list<string>::const_iterator i = dbs.begin(); i != dbs.end(); i++ ) {
            const string db = *i;
            if ( db == "local" )
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
            ScopedTransaction transaction(txn, MODE_IX);
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
            if ( !r->connect(host) ) {
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
            log() << "replSet initial sync failed during oplog application phase, and will retry";

            getGlobalReplicationCoordinator()->setMyLastOptime(OpTime());
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
                    ScopedTransaction transaction(txn, MODE_X);
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
     *
     * Returns a Status with ErrorCode::ShutdownInProgress if the node enters shutdown,
     * ErrorCode::InitialSyncOplogSourceMissing if the node fails to find an sync source, Status::OK
     * if everything worked, and ErrorCode::InitialSyncFailure for all other error cases.
     */
    Status _initialSync() {

        log() << "initial sync pending";

        BackgroundSync* bgsync(BackgroundSync::get());
        OperationContextImpl txn;
        ReplicationCoordinator* replCoord(getGlobalReplicationCoordinator());

        truncateAndResetOplog(&txn, replCoord, bgsync);

        OplogReader r;
        OpTime now(Milliseconds(curTimeMillis64()).total_seconds(), 0);

        while (r.getHost().empty()) {
            // We must prime the sync source selector so that it considers all candidates regardless
            // of oplog position, by passing in "now" as the last op fetched time.
            r.connectToSyncSource(&txn, now, replCoord);
            if (r.getHost().empty()) {
                std::string msg =
                        "no valid sync sources found in current replset to do an initial sync";
                log() << msg;
                return Status(ErrorCodes::InitialSyncOplogSourceMissing, msg);
            }

            if (inShutdown()) {
                return Status(ErrorCodes::ShutdownInProgress, "shutting down");
            }
        }

        InitialSync init(bgsync);
        init.setHostname(r.getHost().toString());

        BSONObj lastOp = r.getLastOp(rsoplog);
        if ( lastOp.isEmpty() ) {
            std::string msg = "initial sync couldn't read remote oplog";
            log() << msg;
            sleepsecs(15);
            return Status(ErrorCodes::InitialSyncFailure, msg);
        }

        if (getGlobalReplicationCoordinator()->getSettings().fastsync) {
            log() << "fastsync: skipping database clone";

            // prime oplog
            try {
                _tryToApplyOpWithRetry(&txn, &init, lastOp);
                _logOpObjRS(&txn, lastOp);
                return Status::OK();
            } catch (DBException& e) {
                // Return if in shutdown
                if (inShutdown()) {
                    return Status(ErrorCodes::ShutdownInProgress, "shutdown in progress");
                }
                throw;
            }
        }

        // Add field to minvalid document to tell us to restart initial sync if we crash
        setInitialSyncFlag(&txn);

        log() << "initial sync drop all databases";
        dropAllDatabasesExceptLocal(&txn);

        log() << "initial sync clone all databases";

        list<string> dbs = r.conn()->getDatabaseNames();

        Cloner cloner;
        if (!_initialSyncClone(&txn, cloner, r.conn()->getServerAddress(), dbs, true)) {
            return Status(ErrorCodes::InitialSyncFailure, "initial sync failed data cloning");
        }

        log() << "initial sync data copy, starting syncup";

        // prime oplog
        _tryToApplyOpWithRetry(&txn, &init, lastOp);
        _logOpObjRS(&txn, lastOp);

        std::string msg = "oplog sync 1 of 3";
        log() << msg;
        if (!_initialSyncApplyOplog(&txn, init, &r)) {
            return Status(ErrorCodes::InitialSyncFailure,
                          str::stream() << "initial sync failed: " << msg);
        }

        // Now we sync to the latest op on the sync target _again_, as we may have recloned ops
        // that were "from the future" compared with minValid. During this second application,
        // nothing should need to be recloned.
        msg = "oplog sync 2 of 3";
        log() << msg;
        if (!_initialSyncApplyOplog(&txn, init, &r)) {
            return Status(ErrorCodes::InitialSyncFailure,
                          str::stream() << "initial sync failed: " << msg);
        }
        // data should now be consistent

        msg = "initial sync building indexes";
        log() << msg;
        if (!_initialSyncClone(&txn, cloner, r.conn()->getServerAddress(), dbs, false)) {
            return Status(ErrorCodes::InitialSyncFailure,
                          str::stream() << "initial sync failed: " << msg);
        }

        msg = "oplog sync 3 of 3";
        log() << msg;

        SyncTail tail(bgsync, multiSyncApply);
        if (!_initialSyncApplyOplog(&txn, tail, &r)) {
            return Status(ErrorCodes::InitialSyncFailure,
                          str::stream() << "initial sync failed: " << msg);
        }
        
        // ---------

        Status status = getGlobalAuthorizationManager()->initialize(&txn);
        if (!status.isOK()) {
            warning() << "Failed to reinitialize auth data after initial sync. " << status;
            return status;
        }

        log() << "initial sync finishing up";

        {
            AutoGetDb autodb(&txn, "local", MODE_X);
            OpTime lastOpTimeWritten(getGlobalReplicationCoordinator()->getMyLastOptime());
            log() << "replSet set minValid=" << lastOpTimeWritten;

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
        bgsync->notify(&txn);

        log() << "initial sync done";
        return Status::OK();
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
                // leave loop when successful
                Status status = _initialSync();
                if (status.isOK()) {
                    break;
                }
                if (status == ErrorCodes::InitialSyncOplogSourceMissing) {
                    sleepsecs(1);
                    return;
                }
            }
            catch(const DBException& e) {
                error() << e ;
                // Return if in shutdown
                if (inShutdown()) {
                    return;
                }
            }

            if (inShutdown()) {
                return;
            }

            error() << "initial sync attempt failed, "
                    << (maxFailedAttempts - ++failedAttempts) << " attempts remaining";
            sleepsecs(5);
        }

        // No need to print a stack
        if (failedAttempts >= maxFailedAttempts) {
            severe() << "The maximum number of retries have been exhausted for initial sync.";
            fassertFailedNoTrace(16233);
        }
    }

} // namespace repl
} // namespace mongo
