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

namespace mongo {
    class BSONObjBuilder;
    
    struct LastError {
        string msg;
        enum UpdatedExistingType { NotUpdate, True, False } updatedExisting;
        int nObjects;
        int nPrev;
        bool valid;
        void raiseError(const char *_msg) {
            reset( true );
            msg = _msg;
        }
        void recordUpdate( bool _updatedExisting ) {
            reset( true );
            nObjects = 1;
            updatedExisting = _updatedExisting ? True : False;
        }
        void recordDelete( int nDeleted ) {
            reset( true );
            nObjects = nDeleted;
        }
        LastError() {
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

    extern boost::thread_specific_ptr<LastError> lastError;

    inline void raiseError(const char *msg) {
        LastError *le = lastError.get();
        if ( le == 0 ) {
            DEV log() << "warning: lastError==0 can't report:" << msg << '\n';
            return;
        }
        le->raiseError(msg);
    }
    
    inline void recordUpdate( bool updatedExisting ) {
        LastError *le = lastError.get();
        if ( le )
            le->recordUpdate( updatedExisting );        
    }

    inline void recordDelete( int nDeleted ) {
        LastError *le = lastError.get();
        if ( le )
            le->recordDelete( nDeleted );        
    }
} // namespace mongo
