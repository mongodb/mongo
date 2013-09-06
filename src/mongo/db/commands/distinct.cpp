// distinct.cpp

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

#include <string>
#include <vector>

#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/instance.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/kill_current_op.h"
#include "mongo/db/query_optimizer.h"
#include "mongo/util/timer.h"

namespace mongo {

    class DistinctCommand : public Command {
    public:
        DistinctCommand() : Command("distinct") {}
        virtual bool slaveOk() const { return true; }
        virtual LockType locktype() const { return READ; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::find);
            out->push_back(Privilege(parseNs(dbname, cmdObj), actions));
        }
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

            NamespaceDetails * d = nsdetails( ns );

            if ( ! d ) {
                result.appendArray( "values" , BSONObj() );
                result.append( "stats" , BSON( "n" << 0 << "nscanned" << 0 << "nscannedObjects" << 0 ) );
                return true;
            }

            shared_ptr<Cursor> cursor;
            if ( ! query.isEmpty() ) {
                cursor = getOptimizedCursor( ns.c_str(), query, BSONObj() );
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
                        cursor = getBestGuessCursor( ns.c_str(), BSONObj(), idx.keyPattern() );
                        if( cursor.get() ) break;
                    }

                }

                if ( ! cursor.get() )
                    cursor = getOptimizedCursor(ns.c_str() , query , BSONObj() );

            }

            
            verify( cursor );
            string cursorName = cursor->toString();
            
            auto_ptr<ClientCursor> cc (new ClientCursor(QueryOption_NoCursorTimeout, cursor, ns));

            // map from indexed field to offset in key object
            map<string, int> indexedFields;  
            if (!cursor->modifiedKeys()) {
                // store index information so we can decide if we can
                // get something out of the index key rather than full object

                int x = 0;
                BSONObjIterator i( cursor->indexKeyPattern() );
                while ( i.more() ) {
                    BSONElement e = i.next();
                    if ( e.isNumber() ) {
                        // only want basic index fields, not "2d" etc
                        indexedFields[e.fieldName()] = x;
                    }
                    x++;
                }
            }

            while ( cursor->ok() ) {
                nscanned++;
                bool loadedRecord = false;

                if ( cursor->currentMatches( &md ) && !cursor->getsetdup( cursor->currLoc() ) ) {
                    n++;

                    BSONObj holder;
                    BSONElementSet temp;
                    // Try to get the record from the key fields.
                    loadedRecord = !getFieldsDotted(indexedFields, cursor, key, temp, holder);

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
    private:
        /**
         * Tries to get the fields from the key first, then the object if the keys don't have it.
         */
        bool getFieldsDotted(const map<string, int>& indexedFields, shared_ptr<Cursor> cursor,
                             const string& name, BSONElementSet &ret, BSONObj& holder) {
            map<string,int>::const_iterator i = indexedFields.find( name );
            if ( i == indexedFields.end() ) {
                cursor->current().getFieldsDotted( name , ret );
                return false;
            }

            int x = i->second;

            holder = cursor->currKey();
            BSONObjIterator it( holder );
            while ( x && it.more() ) {
                it.next();
                x--;
            }
            verify( x == 0 );
            ret.insert( it.next() );
            return true;
        }

    } distinctCmd;

}
