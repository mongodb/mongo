// fts_command_mongod.h

/**
*    Copyright (C) 2012 10gen Inc.
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

#include <algorithm>
#include <string>
#include <vector>

#include "mongo/db/fts/fts_command.h"
#include "mongo/db/fts/fts_search.h"
#include "mongo/db/fts/fts_util.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/projection.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/timer.h"

namespace mongo {

    namespace fts {

        Command::LockType FTSCommand::locktype() const {
            return READ;
        }

        /*
         * Runs the command object cmdobj on the db with name dbname and puts result in result.
         * @param dbname, name of db
         * @param cmdobj, object that contains entire command
         * @param options
         * @param errmsg, reference to error message
         * @param result, reference to builder for result
         * @param fromRepl
         * @return true if successful, false otherwise
         */
        bool FTSCommand::_run(const string& dbname,
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

            Timer comm;

            scoped_ptr<Projection> pr;
            if ( !projection.isEmpty() ) {
                pr.reset( new Projection() );
                pr->init( projection );
            }

            // priority queue for results
            Results results;

            NamespaceDetails * d = nsdetails( ns.c_str() );
            if ( !d ) {
                errmsg = "can't find ns";
                return false;
            }

            vector<int> idxMatches;
            d->findIndexByType( INDEX_NAME, idxMatches );
            if ( idxMatches.size() == 0 ) {
                errmsg = str::stream() << "no text index for: " << ns;
                return false;
            }
            if ( idxMatches.size() > 1 ) {
                errmsg = str::stream() << "too many text index for: " << ns;
                return false;
            }

            const IndexDetails& id = d->idx( idxMatches[0] );
            BSONObj indexPrefix;

            FTSIndex* ftsIndex = static_cast<FTSIndex*>(id.getSpec().getType());
            if ( language == "" ) {
                language = ftsIndex->getFtsSpec().defaultLanguage();
            }
            Status s = ftsIndex->getFtsSpec().getIndexPrefix( filter, &indexPrefix );
            if ( !s.isOK() ) {
                errmsg = s.toString();
                return false;
            }


            FTSQuery query;
            if ( !query.parse( searchString, language ).isOK() ) {
                errmsg = "can't parse search";
                return false;
            }
            result.append( "queryDebugString", query.debugString() );
            result.append( "language", language );

            FTSSearch search( d, id, indexPrefix, query, filter );
            search.go( &results, limit );

            // grab underlying container inside priority queue
            vector<ScoredLocation> r( results.dangerous() );

            // sort results by score (not always in correct order, especially w.r.t. multiterm)
            sort( r.begin(), r.end() );

            // build the results bson array shown to user
            BSONArrayBuilder a( result.subarrayStart( "results" ) );

            int tempSize = 1024 * 1024; // leave a mb for other things
            long long numReturned = 0;

            for ( unsigned n = 0; n < r.size(); n++ ) {
                BSONObj obj = BSONObj::make(r[n].rec);
                BSONObj toSendBack = obj;

                if ( pr ) {
                    toSendBack = pr->transform(obj);
                }

                if ( ( tempSize + toSendBack.objsize() ) >= BSONObjMaxUserSize ) {
                    break;
                }

                BSONObjBuilder x( a.subobjStart() );
                x.append( "score" , r[n].score );
                x.append( "obj", toSendBack );

                BSONObj xobj = x.done();
                tempSize += xobj.objsize();

                numReturned++;
            }

            a.done();

            // returns some stats to the user
            BSONObjBuilder bb( result.subobjStart( "stats" ) );
            bb.appendNumber( "nscanned" , search.getKeysLookedAt() );
            bb.appendNumber( "nscannedObjects" , search.getObjLookedAt() );
            bb.appendNumber( "n" , numReturned );
            bb.appendNumber( "nfound" , r.size() );
            bb.append( "timeMicros", (int)comm.micros() );
            bb.done();

            return true;
        }
    }

}
