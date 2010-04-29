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

#include "../pch.h"
#include "../util/message.h"
#include "../db/jsobj.h"
#include "../db/json.h"
#include <stack>

namespace mongo {

	/** Queries return a cursor object */
    class DBClientCursor : boost::noncopyable {
    public:
		/** If true, safe to call next().  Requests more from server if necessary. */
        bool more();

        /** If true, there is more in our local buffers to be fetched via next(). Returns 
            false when a getMore request back to server would be required.  You can use this 
            if you want to exhaust whatever data has been fetched to the client already but 
            then perhaps stop.
        */
        bool moreInCurrentBatch() { return !_putBack.empty() || pos < nReturned; }

        /** next
		   @return next object in the result cursor.
           on an error at the remote server, you will get back:
             { $err: <string> }
           if you do not want to handle that yourself, call nextSafe().
        */
        BSONObj next();
        
        /** 
            restore an object previously returned by next() to the cursor
         */
        void putBack( const BSONObj &o ) { _putBack.push( o.getOwned() ); }

		/** throws AssertionException if get back { $err : ... } */
        BSONObj nextSafe() {
            BSONObj o = next();
            BSONElement e = o.firstElement();
            if( strcmp(e.fieldName(), "$err") == 0 ) { 
                if( logLevel >= 5 )
                    log() << "nextSafe() error " << o.toString() << endl;
                uassert(13106, "nextSafe() returns $err", false);
            }
            return o;
        }

        /**
           iterate the rest of the cursor and return the number if items
         */
        int itcount(){
            int c = 0;
            while ( more() ){
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
        bool isDead() const {
            return cursorId == 0;
        }

        bool tailable() const {
            return (opts & QueryOption_CursorTailable) != 0;
        }
        
        /** see QueryResult::ResultFlagType (db/dbmessage.h) for flag values 
            mostly these flags are for internal purposes - 
            ResultFlag_ErrSet is the possible exception to that
        */
        bool hasResultFlag( int flag ){
            return (resultFlags & flag) != 0;
        }

        DBClientCursor( DBConnector *_connector, const string &_ns, BSONObj _query, int _nToReturn,
                        int _nToSkip, const BSONObj *_fieldsToReturn, int queryOptions , int bs ) :
                connector(_connector),
                ns(_ns),
                query(_query),
                nToReturn(_nToReturn),
                haveLimit( _nToReturn > 0 && !(queryOptions & QueryOption_CursorTailable)),
                nToSkip(_nToSkip),
                fieldsToReturn(_fieldsToReturn),
                opts(queryOptions),
                batchSize(bs),
                m(new Message()),
                cursorId(),
                nReturned(),
                pos(),
                data(),
                _ownCursor( true ),
                _scopedConn(0){
        }
        
        DBClientCursor( DBConnector *_connector, const string &_ns, long long _cursorId, int _nToReturn, int options ) :
                connector(_connector),
                ns(_ns),
                nToReturn( _nToReturn ),
                haveLimit( _nToReturn > 0 && !(options & QueryOption_CursorTailable)),
                opts( options ),
                m(new Message()),
                cursorId( _cursorId ),
                nReturned(),
                pos(),
                data(),
                _ownCursor( true ),
                _scopedConn(0){
        }            

        virtual ~DBClientCursor();

        long long getCursorId() const { return cursorId; }

        /** by default we "own" the cursor and will send the server a KillCursor
            message when ~DBClientCursor() is called. This function overrides that.
        */
        void decouple() { _ownCursor = false; }
        
        void attach( ScopedDbConnection * conn );
        
    private:
        friend class DBClientBase;
        bool init();        
        int nextBatchSize();
        DBConnector *connector;
        string ns;
        BSONObj query;
        int nToReturn;
        bool haveLimit;
        int nToSkip;
        const BSONObj *fieldsToReturn;
        int opts;
        int batchSize;
        auto_ptr<Message> m;
        stack< BSONObj > _putBack;
        int resultFlags;
        long long cursorId;
        int nReturned;
        int pos;
        const char *data;
        void dataReceived();
        void requestMore();
        bool _ownCursor; // see decouple()
        ScopedDbConnection * _scopedConn;
    };
    
    
} // namespace mongo

#include "undef_macros.h"
