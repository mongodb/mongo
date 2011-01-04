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
#include "../s/shard.h"

namespace mongo {

    void assembleRequest( const string &ns, BSONObj query, int nToReturn, int nToSkip, const BSONObj *fieldsToReturn, int queryOptions, Message &toSend );

    int DBClientCursor::nextBatchSize() {

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
        }
        else {
            BufBuilder b;
            b.appendNum( opts );
            b.appendStr( ns );
            b.appendNum( nToReturn );
            b.appendNum( cursorId );
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

        if (haveLimit) {
            nToReturn -= nReturned;
            assert(nToReturn > 0);
        }
        BufBuilder b;
        b.appendNum(opts);
        b.appendStr(ns);
        b.appendNum(nextBatchSize());
        b.appendNum(cursorId);

        Message toSend;
        toSend.setData(dbGetMore, b.buf(), b.len());
        auto_ptr<Message> response(new Message());

        if ( connector ) {
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

    /** with QueryOption_Exhaust, the server just blasts data at us (marked at end with cursorid==0). */
    void DBClientCursor::exhaustReceiveMore() {
        assert( cursorId && pos == nReturned );
        assert( !haveLimit );
        auto_ptr<Message> response(new Message());
        assert( connector );
        connector->recv(*response);
        m = response;
        dataReceived();
    }

    void DBClientCursor::dataReceived() {
        QueryResult *qr = (QueryResult *) m->singleData();
        resultFlags = qr->resultFlags();

        if ( qr->resultFlags() & ResultFlag_CursorNotFound ) {
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
        _assertIfNull();

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
        DEV _assertIfNull();
        if ( !_putBack.empty() ) {
            BSONObj ret = _putBack.top();
            _putBack.pop();
            return ret;
        }

        uassert(13422, "DBClientCursor next() called but more() is false", pos < nReturned);

        pos++;
        BSONObj o(data);
        data += o.objsize();
        /* todo would be good to make data null at end of batch for safety */
        return o;
    }

    void DBClientCursor::peek(vector<BSONObj>& v, int atMost) {
        int m = atMost;

        /*
        for( stack<BSONObj>::iterator i = _putBack.begin(); i != _putBack.end(); i++ ) {
            if( m == 0 )
                return;
            v.push_back(*i);
            m--;
            n++;
        }
        */

        int p = pos;
        const char *d = data;
        while( m && p < nReturned ) {
            BSONObj o(d);
            d += o.objsize();
            p++;
            m--;
            v.push_back(o);
        }
    }

    void DBClientCursor::attach( AScopedConnection * conn ) {
        assert( _scopedHost.size() == 0 );
        _scopedHost = conn->getHost();
        conn->done();
        connector = 0;
    }

    DBClientCursor::~DBClientCursor() {
        if (!this)
            return;

        DESTRUCTOR_GUARD (

        if ( cursorId && _ownCursor ) {
        BufBuilder b;
        b.appendNum( (int)0 ); // reserved
            b.appendNum( (int)1 ); // number
            b.appendNum( cursorId );

            Message m;
            m.setData( dbKillCursors , b.buf() , b.len() );

            if ( connector ) {
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
