// lasterror.h

/**
*    Copyright (C) 2009 10gen Inc.
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

    typedef pair<int,LastError*> LastErrorStatus ;
    typedef map<int,LastErrorStatus> LastErrorIDMap;

    class LastErrorHolder {
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
        
        void startRequest( Message& m , LastError * connectionOwned = 0 );
    private:
        
        ThreadLocalInt _id;
        boost::thread_specific_ptr<LastError> _tl;
        
        LastErrorIDMap _ids;        
    };
    
    extern LastErrorHolder lastError;

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
