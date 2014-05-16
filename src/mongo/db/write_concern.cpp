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

#include "mongo/base/counter.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/is_master.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/write_concern.h"
#include "mongo/db/stats/timer_stats.h"
#include "mongo/db/write_concern.h"

namespace mongo {

    static TimerStats gleWtimeStats;
    static ServerStatusMetricField<TimerStats> displayGleLatency( "getLastError.wtime", &gleWtimeStats );

    static Counter64 gleWtimeouts;
    static ServerStatusMetricField<Counter64> gleWtimeoutsDisplay( "getLastError.wtimeouts", &gleWtimeouts );

    Status validateWriteConcern( const WriteConcernOptions& writeConcern ) {

        const bool isJournalEnabled = getDur().isDurable();

        if ( writeConcern.syncMode == WriteConcernOptions::JOURNAL && !isJournalEnabled ) {
            return Status( ErrorCodes::BadValue,
                           "cannot use 'j' option when a host does not have journaling enabled" );
        }

        const bool isConfigServer = serverGlobalParams.configsvr;
        const bool isMasterSlaveNode = anyReplEnabled() && !theReplSet;
        const bool isReplSetNode = anyReplEnabled() && theReplSet;

        if ( isConfigServer || ( !isMasterSlaveNode && !isReplSetNode ) ) {

            // Note that config servers can be replicated (have an oplog), but we still don't allow
            // w > 1

            if ( writeConcern.wNumNodes > 1 ) {
                return Status( ErrorCodes::BadValue,
                               string( "cannot use 'w' > 1 " ) +
                               ( isConfigServer ? "on a config server host" :
                                                  "when a host is not replicated" ) );
            }
        }

        if ( !isReplSetNode && !writeConcern.wMode.empty() && writeConcern.wMode != "majority" ) {
            return Status( ErrorCodes::BadValue,
                           string( "cannot use non-majority 'w' mode " ) + writeConcern.wMode
                           + " when a host is not a member of a replica set" );
        }

        return Status::OK();
    }

    void WriteConcernResult::appendTo( const WriteConcernOptions& writeConcern,
                                       BSONObjBuilder* result ) const {

        if ( syncMillis >= 0 )
            result->appendNumber( "syncMillis", syncMillis );

        if ( fsyncFiles >= 0 )
            result->appendNumber( "fsyncFiles", fsyncFiles );

        if ( wTime >= 0 ) {
            if ( wTimedOut )
                result->appendNumber( "waited", wTime );
            else
                result->appendNumber( "wtime", wTime );
        }

        if ( wTimedOut )
            result->appendBool( "wtimeout", true );

        if ( writtenTo.size() )
            result->append( "writtenTo", writtenTo );
        else
            result->appendNull( "writtenTo" );

        if ( err.empty() )
            result->appendNull( "err" );
        else
            result->append( "err", err );

        // *** 2.4 SyncClusterConnection compatibility ***
        // 2.4 expects either fsync'd files, or a "waited" field exist after running an fsync : true
        // GLE, but with journaling we don't actually need to run the fsync (fsync command is
        // preferred in 2.6).  So we add a "waited" field if one doesn't exist.

        if ( writeConcern.syncMode == WriteConcernOptions::FSYNC ) {

            if ( fsyncFiles < 0 && ( wTime < 0 || !wTimedOut ) ) {
                dassert( result->asTempObj()["waited"].eoo() );
                result->appendNumber( "waited", syncMillis );
            }

            dassert( result->asTempObj()["fsyncFiles"].numberInt() > 0 ||
                     !result->asTempObj()["waited"].eoo() );
        }
    }

    Status waitForWriteConcern( OperationContext* txn,
                                const WriteConcernOptions& writeConcern,
                                const OpTime& replOpTime,
                                WriteConcernResult* result ) {

        // We assume all options have been validated earlier, if not, programming error
        dassert( validateWriteConcern( writeConcern ).isOK() );

        // Next handle blocking on disk

        Timer syncTimer;

        switch( writeConcern.syncMode ) {
        case WriteConcernOptions::NONE:
            break;
        case WriteConcernOptions::FSYNC:
            if ( !getDur().isDurable() ) {
                result->fsyncFiles = MemoryMappedFile::flushAll( true );
            }
            else {
                // We only need to commit the journal if we're durable
                txn->recoveryUnit()->awaitCommit();
            }
            break;
        case WriteConcernOptions::JOURNAL:
            txn->recoveryUnit()->awaitCommit();
            break;
        }

        result->syncMillis = syncTimer.millis();

        // Now wait for replication

        if ( replOpTime.isNull() ) {
            // no write happened for this client yet
            return Status::OK();
        }

        if ( writeConcern.wNumNodes <= 1 && writeConcern.wMode.empty() ) {
            // no desired replication check
            return Status::OK();
        }

        if ( !anyReplEnabled() || serverGlobalParams.configsvr ) {
            // no replication check needed (validated above)
            return Status::OK();
        }

        const bool isMasterSlaveNode = anyReplEnabled() && !theReplSet;
        if ( writeConcern.wMode == "majority" && isMasterSlaveNode ) {
            // with master/slave, majority is equivalent to w=1
            return Status::OK();
        }

        // We're sure that replication is enabled and that we have more than one node or a wMode
        TimerHolder gleTimerHolder( &gleWtimeStats );

        // Now we wait for replication
        // Note that replica set stepdowns and gle mode changes are thrown as errors
        // TODO: Make this cleaner
        Status replStatus = Status::OK();
        try {
            while ( 1 ) {

                if ( writeConcern.wNumNodes > 0 ) {
                    if ( opReplicatedEnough( replOpTime, writeConcern.wNumNodes ) ) {
                        break;
                    }
                }
                else if ( opReplicatedEnough( replOpTime, writeConcern.wMode ) ) {
                    break;
                }

                if ( writeConcern.wTimeout > 0 &&
                     gleTimerHolder.millis() >= writeConcern.wTimeout ) {
                    gleWtimeouts.increment();
                    result->err = "timeout";
                    result->wTimedOut = true;
                    replStatus = Status( ErrorCodes::WriteConcernFailed,
                                         "waiting for replication timed out" );
                    break;
                }

                sleepmillis(1);
                txn->checkForInterrupt();
            }
        }
        catch( const AssertionException& ex ) {
            // Our replication state changed while enforcing write concern
            replStatus = ex.toStatus();
        }

        // Add stats
        result->writtenTo = getHostsWrittenTo( replOpTime );
        result->wTime = gleTimerHolder.recordMillis();

        return replStatus;
    }

} // namespace mongo
