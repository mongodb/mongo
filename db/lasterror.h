// lasterror.h

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

#include <boost/thread/tss.hpp>
#undef assert
#define assert MONGO_assert

namespace mongo {
    class BSONObjBuilder;
    class Message;

    struct LastError {
        int code;
        string msg;
        enum UpdatedExistingType { NotUpdate, True, False } updatedExisting;
        /* todo: nObjects should be 64 bit */
        long long nObjects;
        int nPrev;
        bool valid;
        bool overridenById;
        bool disabled;
        void raiseError(int _code , const char *_msg) {
            reset( true );
            code = _code;
            msg = _msg;
        }
        void recordUpdate( bool _updatedExisting, long long nChanged ) {
            reset( true );
            nObjects = nChanged;
            updatedExisting = _updatedExisting ? True : False;
        }
        void recordDelete( long long nDeleted ) {
            reset( true );
            nObjects = nDeleted;
        }
        LastError() {
            overridenById = false;
            reset();
        }
        void reset( bool _valid = false ) {
            code = 0;
            msg.clear();
            updatedExisting = NotUpdate;
            nObjects = 0;
            nPrev = 1;
            valid = _valid;
            disabled = false;
        }
        void appendSelf( BSONObjBuilder &b );
        static LastError noError;
    };

    extern class LastErrorHolder {
    public:
        LastErrorHolder() : _id( 0 ) {}

        LastError * get( bool create = false );

        LastError * _get( bool create = false ); // may return a disabled LastError

        void reset( LastError * le );

        /** ok to call more than once. */
        void initThread();

        /**
         * id of 0 means should use thread local management
         */
        void setID( int id );
        int getID();

        void remove( int id );
        void release();
        
        /** when db receives a message/request, call this */
        void startRequest( Message& m , LastError * connectionOwned );
        LastError * startRequest( Message& m , int clientId = 0 );
        
        // used to disable lastError reporting while processing a killCursors message
        // disable causes get() to return 0.
        LastError *disableForCommand(); // only call once per command invocation!
    private:
        ThreadLocalValue<int> _id;
        boost::thread_specific_ptr<LastError> _tl;
        
        struct Status {
            time_t time;
            LastError *lerr;
        };
        static mongo::mutex _idsmutex;
        map<int,Status> _ids;    
    } lastError;
    
    inline void raiseError(int code , const char *msg) {
        LastError *le = lastError.get();
        if ( le == 0 ) {
            /* might be intentional (non-user thread) */
            DEV log() << "warning dev: lastError==0 won't report:" << msg << endl;
        } else if ( le->disabled ) {
            log() << "lastError disabled, can't report: " << msg << endl;
        } else {
            le->raiseError(code, msg);
        }
    }
    
    inline void recordUpdate( bool updatedExisting, int nChanged ) {
        LastError *le = lastError.get();
        if ( le )
            le->recordUpdate( updatedExisting, nChanged );        
    }

    inline void recordDelete( int nDeleted ) {
        LastError *le = lastError.get();
        if ( le )
            le->recordDelete( nDeleted );        
    }

} // namespace mongo
