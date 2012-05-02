// distinct.cpp

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

namespace mongo {

    class DistinctCommand : public Command {
    public:
        DistinctCommand() : Command("distinct") {}
        virtual bool slaveOk() const { return true; }
        virtual LockType locktype() const { return READ; }
        virtual void help( stringstream &help ) const {
            help << "{ distinct : 'collection name' , key : 'a.b' , query : {} }";
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

            BSONArrayBuilder arr( bb );
            BSONElementSet values;

            long long nscanned = 0; // locations looked at
            long long nscannedObjects = 0; // full objects looked at
            long long n = 0; // matches
            MatchDetails md;

            NamespaceDetails * d = nsdetails( ns.c_str() );

            if ( ! d ) {
                result.appendArray( "values" , BSONObj() );
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
                        if ( values.count( e ) )
                            continue;

                        int now = bb.len();

                        uassert(10044,  "distinct too big, 16mb cap", ( now + e.size() + 1024 ) < bufSize );

                        arr.append( e );
                        BSONElement x( start + now );

                        values.insert( x );
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

            result.appendArray( "values" , arr.done() );

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

    } distinctCmd;

}
