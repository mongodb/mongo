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
#include "mongo/db/commands/server_status.h"
#include "mongo/db/kill_current_op.h"
#include "mongo/db/repl/is_master.h"
#include "mongo/db/repl/replication_server_status.h"
#include "mongo/db/repl/write_concern.h"
#include "mongo/db/stats/timer_stats.h"
#include "mongo/db/write_concern.h"

namespace mongo {

    static TimerStats gleWtimeStats;
    static ServerStatusMetricField<TimerStats> displayGleLatency( "getLastError.wtime", &gleWtimeStats );

    static Counter64 gleWtimeouts;
    static ServerStatusMetricField<Counter64> gleWtimeoutsDisplay( "getLastError.wtimeouts", &gleWtimeouts );

    Status WriteConcernOptions::parse( const BSONObj& obj ) {
        bool j = obj["j"].trueValue();
        bool fsync = obj["fsync"].trueValue();

        if ( j & fsync )
            return Status( ErrorCodes::BadValue, "fsync and j options cannot be used together" );

        if ( j ) {
            syncMode = JOURNAL;
        }
        if ( fsync ) {
            if ( getDur().isDurable() )
                syncMode = JOURNAL;
            else
                syncMode = FSYNC;
        }


        BSONElement e = obj["w"];
        if ( e.isNumber() ) {
            wNumNodes = e.numberInt();
        }
        else if ( e.type() == String ) {
            wMode = e.valuestrsafe();
        }
        else if ( e.eoo() ||
                  e.type() == jstNULL ||
                  e.type() == Undefined ) {
        }
        else {
            return Status( ErrorCodes::BadValue, "w has to be a number or a string" );
        }

        wTimeout = obj["wtimeout"].numberInt();

        return Status::OK();
    }

    void WriteConcernResult::appendTo( BSONObjBuilder* result ) const {
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
    }

    Status waitForWriteConcern( const WriteConcernOptions& writeConcern,
                                const OpTime& replOpTime,
                                WriteConcernResult* result ) {

        // first handle blocking on disk

        Timer syncTimer;
        switch( writeConcern.syncMode ) {
        case WriteConcernOptions::NONE:
            break;
        case WriteConcernOptions::JOURNAL:
            if ( getDur().awaitCommit() ) {
                // success
                break;
            }
            result->err = "nojournal";
            return Status( ErrorCodes::BadValue, "journaling not enabled" );
        case WriteConcernOptions::FSYNC:
            result->fsyncFiles = MemoryMappedFile::flushAll( true );
            break;
        }
        result->syncMillis = syncTimer.millis();

        // now wait for replication

        if ( writeConcern.wNumNodes <= 1 &&
             writeConcern.wMode.empty() ) {
            // no desired replication check
            return Status::OK();
        }

        if ( serverGlobalParams.configsvr ) {
            // config servers have special rules
            if ( writeConcern.wNumNodes > 1 ) {
                result->err = "norepl";
                return Status( ErrorCodes::WriteConcernLegacyOK,
                               "cannot use w > 1 with config servers" );
            }
            if ( writeConcern.wMode == "majority" ) {
                // majority is ok for single nodes, as 1/1 > .5
                return Status::OK();
            }
            result->err = "norepl";
            return Status( ErrorCodes::BadValue,
                           str::stream() << "unknown w mode for config servers "
                           << "(" << writeConcern.wMode << ")" );
        }

        if ( !anyReplEnabled() ) {
            // no replication enabled and not a config server
            // so we handle some simple things, or fail

            if ( writeConcern.wNumNodes > 1 ) {
                result->err = "norepl";
                return Status( ErrorCodes::WriteConcernLegacyOK,
                               str::stream() << "no replication and asked for w > 1 "
                               << "(" << writeConcern.wNumNodes << ")" );
            }
            if ( !writeConcern.wMode.empty() &&
                 writeConcern.wMode != "majority" ) {
                result->err = "norepl";
                return Status( ErrorCodes::WriteConcernLegacyOK,
                               "no replication and asked for w with a mode" );
            }

            // asked for w <= 1 or w=majority
            // so we can just say ok
            return Status::OK();
        }

        bool doTiming = writeConcern.wNumNodes > 1 || !writeConcern.wMode.empty();
        scoped_ptr<TimerHolder> gleTimerHolder( new TimerHolder( doTiming ? &gleWtimeStats : NULL ) );

        if ( replOpTime.isNull() ) {
            // no write happened for this client yet
            return Status::OK();
        }

        if ( !writeConcern.wMode.empty() && !theReplSet ) {
            if ( writeConcern.wMode == "majority" ) {
                // with master/slave, majority is equivilant to w=1
                return Status::OK();
            }
            return Status( ErrorCodes::BadValue,
                           str::stream() << "asked for a w mode with master/slave "
                           << "[" << writeConcern.wMode << "]" );
        }

        // now that we've done the prep, now we actually wait
        while ( 1 ) {

            if ( !_isMaster() ) {
                // this should be in the while loop in case we step down
                return Status( ErrorCodes::NotMaster, "no longer primary" );
            }

            if ( writeConcern.wNumNodes > 0 ) {
                if ( opReplicatedEnough( replOpTime, writeConcern.wNumNodes ) ) {
                    break;
                }
            }
            else if ( opReplicatedEnough( replOpTime, writeConcern.wMode ) ) {
                break;
            }

            if ( writeConcern.wTimeout > 0 &&
                 gleTimerHolder->millis() >= writeConcern.wTimeout ) {
                gleWtimeouts.increment();
                result->wTime = gleTimerHolder->millis();
                result->writtenTo = getHostsWrittenTo( replOpTime );
                result->err = "timeout";
                result->wTimedOut = true;
                return Status( ErrorCodes::WriteConcernLegacyOK,
                               "waiting for replication timed out" );
            }

            sleepmillis(1);
            killCurrentOp.checkForInterrupt();
        }

        if ( doTiming ) {
            result->writtenTo = getHostsWrittenTo( replOpTime );
            result->wTime = gleTimerHolder->recordMillis();
        }

        return Status::OK();
    }

} // namespace mongo
