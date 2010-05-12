// dbclient.cpp - connect to a Mongo database as a database, from C++

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "pch.h"
#include "dbclient.h"
#include "../db/dbmessage.h"
#include "../db/cmdline.h"
#include "connpool.h"

namespace mongo {

    void assembleRequest( const string &ns, BSONObj query, int nToReturn, int nToSkip, const BSONObj *fieldsToReturn, int queryOptions, Message &toSend );

    int DBClientCursor::nextBatchSize(){
        if ( nToReturn == 0 )
            return batchSize;
        if ( batchSize == 0 )
            return nToReturn;
        
        return batchSize < nToReturn ? batchSize : nToReturn;
    }

    bool DBClientCursor::init() {
        Message toSend;
        if ( !cursorId ) {
            assembleRequest( ns, query, nextBatchSize() , nToSkip, fieldsToReturn, opts, toSend );
        } else {
            BufBuilder b;
            b.append( opts );
            b.append( ns.c_str() );
            b.append( nToReturn );
            b.append( cursorId );
            toSend.setData( dbGetMore, b.buf(), b.len() );
        }
        if ( !connector->call( toSend, *m, false ) )
            return false;
        if ( m->empty() )
            return false;
        dataReceived();
        return true;
    }

    void DBClientCursor::requestMore() {
        assert( cursorId && pos == nReturned );

        if (haveLimit){
            nToReturn -= nReturned;
            assert(nToReturn > 0);
        }
        BufBuilder b;
        b.append(opts);
        b.append(ns.c_str());
        b.append(nextBatchSize());
        b.append(cursorId);
        
        Message toSend;
        toSend.setData(dbGetMore, b.buf(), b.len());
        auto_ptr<Message> response(new Message());
        
        if ( connector ){
            connector->call( toSend, *response );
            m = response;
            dataReceived();
        }
        else {
            assert( _scopedHost.size() );
            ScopedDbConnection conn( _scopedHost );
            conn->call( toSend , *response );
            connector = conn.get();
            m = response;
            dataReceived();
            connector = 0;
            conn.done();
        }
        


    }

    void DBClientCursor::dataReceived() {
        QueryResult *qr = (QueryResult *) m->singleData();
        resultFlags = qr->resultFlags();
        
        if ( qr->resultFlags() & QueryResult::ResultFlag_CursorNotFound ) {
            // cursor id no longer valid at the server.
            assert( qr->cursorId == 0 );
            cursorId = 0; // 0 indicates no longer valid (dead)
            if ( ! ( opts & QueryOption_CursorTailable ) )
                throw UserException( 13127 , "getMore: cursor didn't exist on server, possible restart or timeout?" );
        }
        
        if ( cursorId == 0 || ! ( opts & QueryOption_CursorTailable ) ) {
            // only set initially: we don't want to kill it on end of data
            // if it's a tailable cursor
            cursorId = qr->cursorId;
        }

        nReturned = qr->nReturned;
        pos = 0;
        data = qr->data();

        connector->checkResponse( data, nReturned );
        /* this assert would fire the way we currently work:
            assert( nReturned || cursorId == 0 );
        */
    }

    /** If true, safe to call next().  Requests more from server if necessary. */
    bool DBClientCursor::more() {
        if ( !_putBack.empty() )
            return true;
        
        if (haveLimit && pos >= nToReturn)
            return false;

        if ( pos < nReturned )
            return true;

        if ( cursorId == 0 )
            return false;

        requestMore();
        return pos < nReturned;
    }

    BSONObj DBClientCursor::next() {
        assert( more() );
        if ( !_putBack.empty() ) {
            BSONObj ret = _putBack.top();
            _putBack.pop();
            return ret;
        }
        pos++;
        BSONObj o(data);
        data += o.objsize();
        return o;
    }

    void DBClientCursor::attach( ScopedDbConnection * conn ){
        assert( _scopedHost.size() == 0 );
        assert( connector == conn->get() );
        _scopedHost = conn->getHost();
        conn->done();
        connector = 0;
    }



    DBClientCursor::~DBClientCursor() {
        DESTRUCTOR_GUARD (

            if ( cursorId && _ownCursor ) {
                BufBuilder b;
                b.append( (int)0 ); // reserved
                b.append( (int)1 ); // number
                b.append( cursorId );

                Message m;
                m.setData( dbKillCursors , b.buf() , b.len() );
                
                if ( connector ){
                    connector->sayPiggyBack( m );
                }
                else {
                    assert( _scopedHost.size() );
                    ScopedDbConnection conn( _scopedHost );
                    conn->sayPiggyBack( m );
                    conn.done();
                }
            }

        );
    }

    
} // namespace mongo
