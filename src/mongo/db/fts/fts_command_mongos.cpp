// fts_command_mongos.cpp

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
 */

#include <map>
#include <string>
#include <vector>

#include "mongo/pch.h"

#include "mongo/db/fts/fts_command.h"
#include "mongo/s/strategy.h"
#include "mongo/util/timer.h"


namespace mongo {
    namespace fts {

        struct Scored {
            Scored( BSONObj full )
                : full( full ) {
                score = full["score"].numberDouble();
            }
            bool operator<( const Scored& other ) const {
                return other.score < score;
            }
            BSONObj full;
            double score;
        };


        // all grid commands are designed not to lock
        Command::LockType FTSCommand::locktype() const { return NONE; }

        bool FTSCommand::_run(const string& dbName,
                              BSONObj& cmdObj,
                              int cmdOptions,
                              const string& ns,
                              const string& searchString,
                              string language, // "" for not-set
                              int limit,
                              BSONObj& filter,
                              BSONObj& projection,
                              string& errmsg,
                              BSONObjBuilder& result ) {

            Timer timer;

            vector<Strategy::CommandResult> results;
            SHARDED->commandOp( dbName, cmdObj, cmdOptions, ns, filter, &results );

            vector<Scored> all;
            long long nscanned = 0;
            long long nscannedObjects = 0;

            BSONObjBuilder shardStats;

            for ( vector<Strategy::CommandResult>::const_iterator i = results.begin();
                    i != results.end(); ++i ) {
                BSONObj r = i->result;

                LOG(2) << "fts result for shard: " << i->shardTarget << "\n" << r << endl;

                if ( !r["ok"].trueValue() ) {
                    errmsg = str::stream() << "failure on shard: " << i->shardTarget.toString()
                                           << ": " << r["errmsg"];
                    result.append( "rawresult", r );
                    return false;
                }

                if ( r["stats"].isABSONObj() ) {
                    BSONObj x = r["stats"].Obj();
                    nscanned += x["nscanned"].numberLong();
                    nscannedObjects += x["nscannedObjects"].numberLong();

                    shardStats.append( i->shardTarget.getName(), x );
                }

                if ( r["results"].isABSONObj() ) {
                    BSONObjIterator j( r["results"].Obj() );
                    while ( j.more() ) {
                        BSONElement e = j.next();
                        all.push_back( Scored(e.Obj()) );
                    }
                }
            }

            sort( all.begin(), all.end() );
            long long n = 0;
            {
                BSONArrayBuilder arr( result.subarrayStart( "results" ) );
                for ( unsigned i = 0; i < all.size(); i++ ) {
                    arr.append( all[i].full );
                    if ( ++n >= limit )
                        break;
                }
                arr.done();
            }

            {
                BSONObjBuilder stats( result.subobjStart( "stats" ) );
                stats.appendNumber( "nscanned", nscanned );
                stats.appendNumber( "nscannedObjects", nscannedObjects );
                stats.appendNumber( "n", n );
                stats.append( "timeMicros", (int)timer.micros() );

                stats.append( "shards", shardStats.obj() );

                stats.done();
            }

            return true;
        }

    }
}
