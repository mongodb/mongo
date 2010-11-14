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

#include "pch.h"
#include "../commands.h"
#include "../instance.h"
#include "../queryoptimizer.h"
#include "../clientcursor.h"

namespace mongo {

    class DistinctCommand : public Command {
    public:
        DistinctCommand() : Command("distinct"){}
        virtual bool slaveOk() const { return true; }
        virtual LockType locktype() const { return READ; } 
        virtual void help( stringstream &help ) const {
            help << "{ distinct : 'collection name' , key : 'a.b' , query : {} }";
        }

        bool run(const string& dbname, BSONObj& cmdObj, string& errmsg, BSONObjBuilder& result, bool fromRepl ){
            string ns = dbname + '.' + cmdObj.firstElement().valuestr();
            
            string key = cmdObj["key"].valuestrsafe();
            BSONObj keyPattern = BSON( key << 1 );

            BSONObj query = getQuery( cmdObj );
            
            int bufSize = BSONObjMaxUserSize - 4096;
            BufBuilder bb( bufSize );
            char * start = bb.buf();
                
            BSONArrayBuilder arr( bb );
            BSONElementSet values;
            
            long long nscanned = 0;
            long long n = 0;
            MatchDetails md;

            shared_ptr<Cursor> cursor = bestGuessCursor(ns.c_str() , query , BSONObj() );
            scoped_ptr<ClientCursor> cc (new ClientCursor(QueryOption_NoCursorTimeout, cursor, ns));
            
            while ( cursor->ok() ){
                nscanned++;
                bool loadedObject = false;
                
                if ( !cursor->matcher() || cursor->matcher()->matchesCurrent( cursor.get() , &md ) ){
                    loadedObject = true;
                    BSONObj o = cursor->current();

                    BSONElementSet temp;
                    o.getFieldsDotted( key, temp );
                    
                    for ( BSONElementSet::iterator i=temp.begin(); i!=temp.end(); ++i ){
                        BSONElement e = *i;
                        if ( values.count( e ) )
                            continue;
                        
                        int now = bb.len();

                        uassert(10044,  "distinct too big, 4mb cap", ( now + e.size() + 1024 ) < bufSize );

                        arr.append( e );
                        BSONElement x( start + now );

                        values.insert( x );
                    }
                }

                if ( loadedObject || md.loadedObject ) 
                    n++;

                cursor->advance();

                if (!cc->yieldSometimes())
                    break;

                RARELY killCurrentOp.checkForInterrupt();
            }

            assert( start == bb.buf() );
            
            result.appendArray( "values" , arr.done() );
            
            {
                BSONObjBuilder b;
                b.appendNumber( "n" , n );
                b.appendNumber( "nscanned" , nscanned );
                result.append( "stats" , b.obj() );
            }
            
            return true;
        }

    } distinctCmd;

}
