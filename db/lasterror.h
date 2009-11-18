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
#define assert xassert

namespace mongo {
    class BSONObjBuilder;
    class Message;

    struct LastError {
        string msg;
        enum UpdatedExistingType { NotUpdate, True, False } updatedExisting;
        /* todo: nObjects should be 64 bit */
        int nObjects;
        int nPrev;
        bool valid;
        bool overridenById;
        void raiseError(const char *_msg) {
            reset( true );
            msg = _msg;
        }
        void recordUpdate( bool _updatedExisting, int nChanged ) {
            reset( true );
            nObjects = nChanged;
            updatedExisting = _updatedExisting ? True : False;
        }
        void recordDelete( int nDeleted ) {
            reset( true );
            nObjects = nDeleted;
        }
        LastError() {
            overridenById = false;
            reset();
        }
        void reset( bool _valid = false ) {
            msg.clear();
            updatedExisting = NotUpdate;
            nObjects = 0;
            nPrev = 1;
            valid = _valid;
        }
        void appendSelf( BSONObjBuilder &b );
        static LastError noError;
    };

    extern class LastErrorHolder {
    public:
        LastErrorHolder() : _id( 0 ){}

        LastError * get( bool create = false );

        void reset( LastError * le );
        
        /**
         * id of 0 means should use thread local management
         */
        void setID( int id );
        int getID();

        void remove( int id );
        void release();
        
        /** when db receives a message/request, call this */
        void startRequest( Message& m , LastError * connectionOwned );
        void startRequest( Message& m );
    private:
        ThreadLocalValue<int> _id;
        boost::thread_specific_ptr<LastError> _tl;
        
        struct Status {
            time_t time;
            LastError *lerr;
        };
        static boost::mutex _idsmutex;
        map<int,Status> _ids;        
    } lastError;
    
    inline void raiseError(const char *msg) {
        LastError *le = lastError.get();
        if ( le == 0 ) {
            DEV log() << "warning: lastError==0 can't report:" << msg << '\n';
            return;
        }
        le->raiseError(msg);
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
