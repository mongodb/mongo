// distinctcount.cpp

/**
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

//#include "pch.h"
#include "../commands.h"
#include "../instance.h"
#include "../clientcursor.h"
#include "../../util/timer.h"
#include <set>

namespace mongo {

    class DistinctcountCommand : public Command {
    public:
        DistinctcountCommand() : Command("distinctcount") {}
        virtual bool slaveOk() const { return true; }
        virtual LockType locktype() const { return READ; }
        virtual void help( stringstream &help ) const {
            help << "{ distinctcount : 'collection name' , key : 'a.b' , query : {} }";
        }

        bool run(const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
            Timer t;
            string ns = dbname + '.' + cmdObj.firstElement().valuestr();

            string key = cmdObj["key"].valuestrsafe();
            BSONObj keyPattern = BSON( key << 1 );

            BSONObj query = getQuery( cmdObj );

            int bufSize = BSONObjMaxUserSize - 4096;
            BufBuilder bb( bufSize );
            char * start = bb.buf();

            std::set<string> STDElementSet;

            long long nscanned = 0; // locations looked at
            long long nscannedObjects = 0; // full objects looked at
            long long n = 0; // matches
            MatchDetails md;

            NamespaceDetails * d = nsdetails( ns.c_str() );

            if ( ! d ) {
                result.appendNumber( "distinctcount", 0 );
                result.append( "stats" , BSON( "n" << 0 << "nscanned" << 0 << "nscannedObjects" << 0 ) );
                return true;
            }

            shared_ptr<Cursor> cursor;
            if ( ! query.isEmpty() ) {
                cursor = NamespaceDetailsTransient::getCursor(ns.c_str() , query , BSONObj() );
            }
            else {

                // query is empty, so lets see if we can find an index
                // with the key so we don't have to hit the raw data
                NamespaceDetails::IndexIterator ii = d->ii();
                while ( ii.more() ) {
                    IndexDetails& idx = ii.next();

                    if ( d->isMultikey( ii.pos() - 1 ) )
                        continue;

                    if ( idx.inKeyPattern( key ) ) {
                        cursor = NamespaceDetailsTransient::bestGuessCursor( ns.c_str() ,
                                                                            BSONObj() ,
                                                                            idx.keyPattern() );
                        if( cursor.get() ) break;
                    }

                }

                if ( ! cursor.get() )
                    cursor = NamespaceDetailsTransient::getCursor(ns.c_str() , query , BSONObj() );

            }

            verify( cursor );
            string cursorName = cursor->toString();
            
            auto_ptr<ClientCursor> cc (new ClientCursor(QueryOption_NoCursorTimeout, cursor, ns));

            while ( cursor->ok() ) {
                nscanned++;
                bool loadedRecord = false;

                if ( cursor->currentMatches( &md ) && !cursor->getsetdup( cursor->currLoc() ) ) {
                    n++;

                    BSONObj holder;
                    BSONElementSet temp;
                    loadedRecord = ! cc->getFieldsDotted( key , temp, holder );

                    for ( BSONElementSet::iterator i=temp.begin(); i!=temp.end(); ++i ) {
                        BSONElement e = *i;

                        string tempString;
                        tempString = e.toString(false, true);

                        if ( STDElementSet.count( tempString ) )
                            continue;

                        STDElementSet.insert( tempString );
                        
                        /**
                         * 16mb cap removed
                         * distinct count may excede this ammout while populating STDElementSet. 
                         * The returned value will be much lower than 16mb (just STDElementSet.size())
                         * uassert(16485,  "distinctcount too big, 16mb cap", ( now + e.size() + 1024 ) < bufSize );
                         */
                    }
                }

                if ( loadedRecord || md.hasLoadedRecord() )
                    nscannedObjects++;

                cursor->advance();

                if (!cc->yieldSometimes( ClientCursor::MaybeCovered )) {
                    cc.release();
                    break;
                }

                RARELY killCurrentOp.checkForInterrupt();
            }

            verify( start == bb.buf() );

            result.appendNumber( "distinctcount", STDElementSet.size() );
            STDElementSet.clear();

            {
                BSONObjBuilder b;
                b.appendNumber( "n" , n );
                b.appendNumber( "nscanned" , nscanned );
                b.appendNumber( "nscannedObjects" , nscannedObjects );
                b.appendNumber( "timems" , t.millis() );
                b.append( "cursor" , cursorName );
                result.append( "stats" , b.obj() );
            }

            return true;
        }

    } distinctcountCmd;

}
