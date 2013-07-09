// file dbclientcursor.h

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

#pragma once

#include "mongo/pch.h"

#include <stack>

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/util/net/message.h"

namespace mongo {

    class AScopedConnection;

    /** for mock purposes only -- do not create variants of DBClientCursor, nor hang code here 
        @see DBClientMockCursor
     */
    class DBClientCursorInterface : boost::noncopyable {
    public:
        virtual ~DBClientCursorInterface() {}
        virtual bool more() = 0;
        virtual BSONObj next() = 0;
        // TODO bring more of the DBClientCursor interface to here
    protected:
        DBClientCursorInterface() {}
    };

    /** Queries return a cursor object */
    class DBClientCursor : public DBClientCursorInterface {
    public:
        /** If true, safe to call next().  Requests more from server if necessary. */
        bool more();

        /** If true, there is more in our local buffers to be fetched via next(). Returns
            false when a getMore request back to server would be required.  You can use this
            if you want to exhaust whatever data has been fetched to the client already but
            then perhaps stop.
        */
        int objsLeftInBatch() const { _assertIfNull(); return _putBack.size() + batch.nReturned - batch.pos; }
        bool moreInCurrentBatch() { return objsLeftInBatch() > 0; }

        /** next
           @return next object in the result cursor.
           on an error at the remote server, you will get back:
             { $err: <string> }
           if you do not want to handle that yourself, call nextSafe().

           Warning: The returned BSONObj will become invalid after the next batch
               is fetched or when this cursor is destroyed.
        */
        BSONObj next();

        /**
            restore an object previously returned by next() to the cursor
         */
        void putBack( const BSONObj &o ) { _putBack.push( o.getOwned() ); }

        /** throws AssertionException if get back { $err : ... } */
        BSONObj nextSafe() {
            BSONObj o = next();
            if( strcmp(o.firstElementFieldName(), "$err") == 0 ) {
                string s = "nextSafe(): " + o.toString();
                LOG(5) << s;
                uasserted(13106, s);
            }
            return o;
        }

        /** peek ahead at items buffered for future next() calls.
            never requests new data from the server.  so peek only effective
            with what is already buffered.
            WARNING: no support for _putBack yet!
        */
        void peek(vector<BSONObj>&, int atMost);

        // Peeks at first element, if exists
        BSONObj peekFirst();

        /**
         * peek ahead and see if an error occurred, and get the error if so.
         */
        bool peekError(BSONObj* error = NULL);

        /**
           iterate the rest of the cursor and return the number if items
         */
        int itcount() {
            int c = 0;
            while ( more() ) {
                next();
                c++;
            }
            return c;
        }

        /** cursor no longer valid -- use with tailable cursors.
           note you should only rely on this once more() returns false;
           'dead' may be preset yet some data still queued and locally
           available from the dbclientcursor.
        */
        bool isDead() const { return  !this || cursorId == 0; }

        bool tailable() const { return (opts & QueryOption_CursorTailable) != 0; }

        /** see ResultFlagType (constants.h) for flag values
            mostly these flags are for internal purposes -
            ResultFlag_ErrSet is the possible exception to that
        */
        bool hasResultFlag( int flag ) {
            _assertIfNull();
            return (resultFlags & flag) != 0;
        }

        /// Change batchSize after construction. Can change after requesting first batch.
        void setBatchSize(int newBatchSize) { batchSize = newBatchSize; }

        DBClientCursor( DBClientBase* client, const string &_ns, BSONObj _query, int _nToReturn,
                        int _nToSkip, const BSONObj *_fieldsToReturn, int queryOptions , int bs ) :
            _client(client),
            ns(_ns),
            query(_query),
            nToReturn(_nToReturn),
            haveLimit( _nToReturn > 0 && !(queryOptions & QueryOption_CursorTailable)),
            nToSkip(_nToSkip),
            fieldsToReturn(_fieldsToReturn),
            opts(queryOptions),
            batchSize(bs==1?2:bs),
            resultFlags(0),
            cursorId(),
            _ownCursor( true ),
            wasError( false ) {
            _finishConsInit();
        }

        DBClientCursor( DBClientBase* client, const string &_ns, long long _cursorId, int _nToReturn, int options ) :
            _client(client),
            ns(_ns),
            nToReturn( _nToReturn ),
            haveLimit( _nToReturn > 0 && !(options & QueryOption_CursorTailable)),
            nToSkip(0),
            fieldsToReturn(0),
            opts( options ),
            batchSize(0),
            resultFlags(0),
            cursorId(_cursorId),
            _ownCursor(true),
            wasError(false) {
            _finishConsInit();
        }

        virtual ~DBClientCursor();

        long long getCursorId() const { return cursorId; }

        /** by default we "own" the cursor and will send the server a KillCursor
            message when ~DBClientCursor() is called. This function overrides that.
        */
        void decouple() { _ownCursor = false; }

        void attach( AScopedConnection * conn );

        string originalHost() const { return _originalHost; }

        string getns() const { return ns; }

        Message* getMessage(){ return batch.m.get(); }

        /**
         * Used mainly to run commands on connections that doesn't support lazy initialization and
         * does not support commands through the call interface.
         *
         * @param cmd The BSON representation of the command to send.
         *
         * @return true if command was sent successfully
         */
        bool initCommand();

        /**
         * actually does the query
         */
        bool init();

        void initLazy( bool isRetry = false );
        bool initLazyFinish( bool& retry );

        class Batch : boost::noncopyable { 
            friend class DBClientCursor;
            auto_ptr<Message> m;
            int nReturned;
            int pos;
            const char *data;
        public:
            Batch() : m( new Message() ), nReturned(), pos(), data() { }
        };

    private:
        friend class DBClientBase;
        friend class DBClientConnection;

        int nextBatchSize();
        void _finishConsInit();
        
        Batch batch;
        DBClientBase* _client;
        string _originalHost;
        string ns;
        BSONObj query;
        int nToReturn;
        bool haveLimit;
        int nToSkip;
        const BSONObj *fieldsToReturn;
        int opts;
        int batchSize;
        stack< BSONObj > _putBack;
        int resultFlags;
        long long cursorId;
        bool _ownCursor; // see decouple()
        string _scopedHost;
        string _lazyHost;
        bool wasError;

        void dataReceived() { bool retry; string lazyHost; dataReceived( retry, lazyHost ); }
        void dataReceived( bool& retry, string& lazyHost );
        void requestMore();
        void exhaustReceiveMore(); // for exhaust

        // Don't call from a virtual function
        void _assertIfNull() const { uassert(13348, "connection died", this); }

        // non-copyable , non-assignable
        DBClientCursor( const DBClientCursor& );
        DBClientCursor& operator=( const DBClientCursor& );

        // init pieces
        void _assembleInit( Message& toSend );
    };

    /** iterate over objects in current batch only - will not cause a network call
     */
    class DBClientCursorBatchIterator {
    public:
        DBClientCursorBatchIterator( DBClientCursor &c ) : _c( c ), _n() {}
        bool moreInCurrentBatch() { return _c.moreInCurrentBatch(); }
        BSONObj nextSafe() {
            massert( 13383, "BatchIterator empty", moreInCurrentBatch() );
            ++_n;
            return _c.nextSafe();
        }
        int n() const { return _n; }
    private:
        DBClientCursor &_c;
        int _n;
    };

} // namespace mongo

