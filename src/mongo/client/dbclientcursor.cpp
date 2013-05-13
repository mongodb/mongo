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

#include "mongo/pch.h"

#include "mongo/client/dbclientcursor.h"

#include "mongo/client/connpool.h"
#include "mongo/db/cmdline.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/namespace_string.h"
#include "mongo/s/shard.h"
#include "mongo/s/stale_exception.h"  // for RecvStaleConfigException

namespace mongo {

    void assembleRequest( const string &ns, BSONObj query, int nToReturn, int nToSkip, const BSONObj *fieldsToReturn, int queryOptions, Message &toSend );

    void DBClientCursor::_finishConsInit() {
        _originalHost = _client->toString();
    }

    int DBClientCursor::nextBatchSize() {

        if ( nToReturn == 0 )
            return batchSize;

        if ( batchSize == 0 )
            return nToReturn;

        return batchSize < nToReturn ? batchSize : nToReturn;
    }

    void DBClientCursor::_assembleInit( Message& toSend ) {
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
    }

    bool DBClientCursor::init() {
        Message toSend;
        _assembleInit( toSend );
        verify( _client );
        if ( !_client->call( toSend, *batch.m, false, &_originalHost ) ) {
            // log msg temp?
            log() << "DBClientCursor::init call() failed" << endl;
            return false;
        }
        if ( batch.m->empty() ) {
            // log msg temp?
            log() << "DBClientCursor::init message from call() was empty" << endl;
            return false;
        }
        dataReceived();
        return true;
    }
    
    void DBClientCursor::initLazy( bool isRetry ) {
        massert( 15875 , "DBClientCursor::initLazy called on a client that doesn't support lazy" , _client->lazySupported() );
        Message toSend;
        _assembleInit( toSend );
        _client->say( toSend, isRetry, &_originalHost );
    }

    bool DBClientCursor::initLazyFinish( bool& retry ) {

        bool recvd = _client->recv( *batch.m );

        // If we get a bad response, return false
        if ( ! recvd || batch.m->empty() ) {

            if( !recvd )
                log() << "DBClientCursor::init lazy say() failed" << endl;
            if( batch.m->empty() )
                log() << "DBClientCursor::init message from say() was empty" << endl;

            _client->checkResponse( NULL, -1, &retry, &_lazyHost );

            return false;

        }

        dataReceived( retry, _lazyHost );
        return ! retry;
    }

    bool DBClientCursor::initCommand(){
        BSONObj res;

        bool ok = _client->runCommand( nsGetDB( ns ), query, res, opts );
        replyToQuery( 0, *batch.m, res );
        dataReceived();

        return ok;
    }

    void DBClientCursor::requestMore() {
        verify( cursorId && batch.pos == batch.nReturned );

        if (haveLimit) {
            nToReturn -= batch.nReturned;
            verify(nToReturn > 0);
        }
        BufBuilder b;
        b.appendNum(opts);
        b.appendStr(ns);
        b.appendNum(nextBatchSize());
        b.appendNum(cursorId);

        Message toSend;
        toSend.setData(dbGetMore, b.buf(), b.len());
        auto_ptr<Message> response(new Message());

        if ( _client ) {
            _client->call( toSend, *response );
            this->batch.m = response;
            dataReceived();
        }
        else {
            verify( _scopedHost.size() );
            ScopedDbConnection conn(_scopedHost);
            conn->call( toSend , *response );
            _client = conn.get();
            this->batch.m = response;
            dataReceived();
            _client = 0;
            conn.done();
        }
    }

    /** with QueryOption_Exhaust, the server just blasts data at us (marked at end with cursorid==0). */
    void DBClientCursor::exhaustReceiveMore() {
        verify( cursorId && batch.pos == batch.nReturned );
        verify( !haveLimit );
        auto_ptr<Message> response(new Message());
        verify( _client );
        if (!_client->recv(*response)) {
            uasserted(16465, "recv failed while exhausting cursor");
        }
        batch.m = response;
        dataReceived();
    }

    void DBClientCursor::dataReceived( bool& retry, string& host ) {

        QueryResult *qr = (QueryResult *) batch.m->singleData();
        resultFlags = qr->resultFlags();

        if ( qr->resultFlags() & ResultFlag_ErrSet ) {
            wasError = true;
        }

        if ( qr->resultFlags() & ResultFlag_CursorNotFound ) {
            // cursor id no longer valid at the server.
            verify( qr->cursorId == 0 );
            cursorId = 0; // 0 indicates no longer valid (dead)
            if ( ! ( opts & QueryOption_CursorTailable ) )
                throw UserException( 13127 , "getMore: cursor didn't exist on server, possible restart or timeout?" );
        }

        if ( cursorId == 0 || ! ( opts & QueryOption_CursorTailable ) ) {
            // only set initially: we don't want to kill it on end of data
            // if it's a tailable cursor
            cursorId = qr->cursorId;
        }

        batch.nReturned = qr->nReturned;
        batch.pos = 0;
        batch.data = qr->data();

        _client->checkResponse( batch.data, batch.nReturned, &retry, &host ); // watches for "not master"

        if( qr->resultFlags() & ResultFlag_ShardConfigStale ) {
            BSONObj error;
            verify( peekError( &error ) );
            throw RecvStaleConfigException( (string)"stale config on lazy receive" + causedBy( getErrField( error ) ), error );
        }

        /* this assert would fire the way we currently work:
            verify( nReturned || cursorId == 0 );
        */
    }

    /** If true, safe to call next().  Requests more from server if necessary. */
    bool DBClientCursor::more() {
        _assertIfNull();

        if ( !_putBack.empty() )
            return true;

        if (haveLimit && batch.pos >= nToReturn)
            return false;

        if ( batch.pos < batch.nReturned )
            return true;

        if ( cursorId == 0 )
            return false;

        requestMore();
        return batch.pos < batch.nReturned;
    }

    BSONObj DBClientCursor::next() {
        DEV _assertIfNull();
        if ( !_putBack.empty() ) {
            BSONObj ret = _putBack.top();
            _putBack.pop();
            return ret;
        }

        uassert(13422, "DBClientCursor next() called but more() is false", batch.pos < batch.nReturned);

        batch.pos++;
        BSONObj o(batch.data);
        batch.data += o.objsize();
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

        int p = batch.pos;
        const char *d = batch.data;
        while( m && p < batch.nReturned ) {
            BSONObj o(d);
            d += o.objsize();
            p++;
            m--;
            v.push_back(o);
        }
    }
    
    BSONObj DBClientCursor::peekFirst(){
        vector<BSONObj> v;
        peek( v, 1 );

        if( v.size() > 0 ) return v[0];
        else return BSONObj();
    }

    bool DBClientCursor::peekError(BSONObj* error){
        if( ! wasError ) return false;

        vector<BSONObj> v;
        peek(v, 1);

        verify( v.size() == 1 );
        verify( hasErrField( v[0] ) );

        if( error ) *error = v[0].getOwned();
        return true;
    }

    void DBClientCursor::attach( AScopedConnection * conn ) {
        verify( _scopedHost.size() == 0 );
        verify( conn );
        verify( conn->get() );

        if ( conn->get()->type() == ConnectionString::SET ||
             conn->get()->type() == ConnectionString::SYNC ) {
            if( _lazyHost.size() > 0 )
                _scopedHost = _lazyHost;
            else if( _client )
                _scopedHost = _client->getServerAddress();
            else
                massert(14821, "No client or lazy client specified, cannot store multi-host connection.", false);
        }
        else {
            _scopedHost = conn->getHost();
        }

        conn->done();
        _client = 0;
        _lazyHost = "";
    }

    DBClientCursor::~DBClientCursor() {
        if (!this)
            return;

        DESTRUCTOR_GUARD (

        if ( cursorId && _ownCursor && ! inShutdown() ) {
            BufBuilder b;
            b.appendNum( (int)0 ); // reserved
            b.appendNum( (int)1 ); // number
            b.appendNum( cursorId );
            
            Message m;
            m.setData( dbKillCursors , b.buf() , b.len() );

            if ( _client ) {

                // Kill the cursor the same way the connection itself would.  Usually, non-lazily
                if( DBClientConnection::getLazyKillCursor() )
                    _client->sayPiggyBack( m );
                else
                    _client->say( m );

            }
            else {
                verify( _scopedHost.size() );
                ScopedDbConnection conn(_scopedHost);

                if( DBClientConnection::getLazyKillCursor() )
                    conn->sayPiggyBack( m );
                else
                    conn->say( m );

                conn.done();
            }
        }

        );
    }


} // namespace mongo
