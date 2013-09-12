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
#include "mongo/db/lasterror.h"
#include "mongo/db/kill_current_op.h"
#include "mongo/db/repl/is_master.h"
#include "mongo/db/repl/replication_server_status.h"
#include "mongo/db/repl/write_concern.h"
#include "mongo/db/stats/timer_stats.h"

namespace mongo {

    static TimerStats gleWtimeStats;
    static ServerStatusMetricField<TimerStats> displayGleLatency( "getLastError.wtime", &gleWtimeStats );

    static Counter64 gleWtimeouts;
    static ServerStatusMetricField<Counter64> gleWtimeoutsDisplay( "getLastError.wtimeouts", &gleWtimeouts );

    bool waitForWriteConcern(const BSONObj& cmdObj,
                             bool err,
                             BSONObjBuilder* result,
                             string* errmsg) {
        Client& c = cc();

        if ( cmdObj["j"].trueValue() ) {
            if( !getDur().awaitCommit() ) {
                // --journal is off
                result->append("jnote", "journaling not enabled on this server");
                // Set the err field, if the result document doesn't already have it set.
                if ( !err ) {
                    result->append( "err", "nojournal" );
                }
                return true;
            }
            if( cmdObj["fsync"].trueValue() ) {
                *errmsg = "fsync and j options are not used together";
                return false;
            }
        }
        else if ( cmdObj["fsync"].trueValue() ) {
            Timer t;
            if( !getDur().awaitCommit() ) {
                // if get here, not running with --journal
                log() << "fsync from getlasterror" << endl;
                result->append( "fsyncFiles" , MemoryMappedFile::flushAll( true ) );
            }
            else {
                // this perhaps is temp.  how long we wait for the group commit to occur.
                result->append( "waited", t.millis() );
            }
        }

        if ( err ) {
            // doesn't make sense to wait for replication
            // if there was an error
            return true;
        }

        BSONElement e = cmdObj["w"];
        if ( e.ok() ) {

            if ( cmdLine.configsvr && (!e.isNumber() || e.numberInt() > 1) ) {
                // w:1 on config servers should still work, but anything greater than that
                // should not.
                result->append( "wnote", "can't use w on config servers" );
                result->append( "err", "norepl" );
                return true;
            }

            int timeout = cmdObj["wtimeout"].numberInt();
            scoped_ptr<TimerHolder> gleTimerHolder;
            bool doTiming = false;
            if ( e.isNumber() ) {
                doTiming = e.numberInt() > 1;
            }
            else if ( e.type() == String ) {
                doTiming = true;
            }
            if ( doTiming ) {
                gleTimerHolder.reset( new TimerHolder( &gleWtimeStats ) );
            }
            else {
                gleTimerHolder.reset( new TimerHolder( NULL ) );
            }

            long long passes = 0;
            char buf[32];
            OpTime op(c.getLastOp());

            if ( op.isNull() ) {
                if ( anyReplEnabled() ) {
                    result->append( "wnote" , "no write has been done on this connection" );
                }
                else if ( e.isNumber() && e.numberInt() <= 1 ) {
                    // don't do anything
                    // w=1 and no repl, so this is fine
                }
                else if (e.type() == mongo::String &&
                         str::equals(e.valuestrsafe(), "majority")) {
                    // don't do anything
                    // w=majority and no repl, so this is fine
                }
                else {
                    // w=2 and no repl
                    stringstream errmsg;
                    errmsg << "no replication has been enabled, so w=" <<
                              e.toString(false) << " won't work";
                    result->append( "wnote" , errmsg.str() );
                    result->append( "err", "norepl" );
                    return true;
                }

                result->appendNull( "err" );
                return true;
            }

            if ( !theReplSet && !e.isNumber() ) {
                // For master/slave deployments that receive w:"majority" or some other named
                // write concern mode, treat it like w:1 and include a note.
                result->append( "wnote", "cannot use non integer w values for non-replica sets" );
                result->appendNull( "err" );
                return true;
            }

            while ( 1 ) {

                if ( !_isMaster() ) {
                    // this should be in the while loop in case we step down
                    *errmsg = "not master";
                    result->append( "wnote", "no longer primary" );
                    result->append( "code" , 10990 );
                    return false;
                }

                // check this first for w=0 or w=1
                if ( opReplicatedEnough( op, e ) ) {
                    break;
                }

                // if replication isn't enabled (e.g., config servers)
                if ( ! anyReplEnabled() ) {
                    result->append( "err", "norepl" );
                    return true;
                }


                if ( timeout > 0 && gleTimerHolder->millis() >= timeout ) {
                    gleWtimeouts.increment();
                    result->append( "wtimeout" , true );
                    *errmsg = "timed out waiting for slaves";
                    result->append( "waited" , gleTimerHolder->millis() );
                    result->append("writtenTo", getHostsWrittenTo(op));
                    result->append( "err" , "timeout" );
                    return true;
                }

                verify( sprintf( buf , "w block pass: %lld" , ++passes ) < 30 );
                c.curop()->setMessage( buf );
                sleepmillis(1);
                killCurrentOp.checkForInterrupt();
            }

            if ( doTiming ) {
                result->append("writtenTo", getHostsWrittenTo(op));
                int myMillis = gleTimerHolder->recordMillis();
                result->appendNumber( "wtime" , myMillis );
            }
        }

        result->appendNull( "err" );
        return true;
    }

} // namespace mongo
