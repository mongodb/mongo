// lockstat.cpp

/**
*    Copyright (C) 2008 10gen Inc.
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


#include "mongo/pch.h"

#include "mongo/db/lockstat.h"

#include "mongo/db/jsobj.h"

namespace mongo { 

    BSONObj LockStat::report() const { 
        BSONObjBuilder b;

        BSONObjBuilder t( b.subobjStart( "timeLockedMicros" ) );
        _append( b , timeLocked );
        t.done();
        
        BSONObjBuilder a( b.subobjStart( "timeAcquiringMicros" ) );
        _append( a , timeAcquiring );
        a.done();
        
        return b.obj();
    }

    void LockStat::report( StringBuilder& builder ) const {
        bool prefixPrinted = false;
        for ( int i=0; i < N; i++ ) {
            if ( timeLocked[i].load() == 0 )
                continue;

            if ( ! prefixPrinted ) {
                builder << "locks(micros)";
                prefixPrinted = true;
            }

            builder << ' ' << nameFor( i ) << ':' << timeLocked[i].load();
        }
        
    }

    void LockStat::_append( BSONObjBuilder& builder, const AtomicInt64* data ) {
        if ( data[0].load() || data[1].load() ) {
            builder.append( "R" , data[0].load() );
            builder.append( "W" , data[1].load() );
        }
        
        if ( data[2].load() || data[3].load() ) {
            builder.append( "r" , data[2].load() );
            builder.append( "w" , data[3].load() );
        }
    }

    unsigned LockStat::mapNo(char type) {
        switch( type ) { 
        case 'R' : return 0;
        case 'W' : return 1;
        case 'r' : return 2;
        case 'w' : return 3;
        default: ;
        }
        fassert(16146,false);
        return 0;
    }

    char LockStat::nameFor(unsigned offset) {
        switch ( offset ) {
        case 0: return 'R';
        case 1: return 'W';
        case 2: return 'r';
        case 3: return 'w';
        }
        fassertFailed(16339);
    }


    void LockStat::recordAcquireTimeMicros( char type , long long micros ) {
        timeAcquiring[mapNo(type)].fetchAndAdd( micros );
    }
    void LockStat::recordLockTimeMicros( char type , long long micros ) {
        timeLocked[mapNo(type)].fetchAndAdd( micros );
    }

    void LockStat::reset() {
        for ( int i = 0; i < N; i++ ) {
            timeAcquiring[i].store(0);
            timeLocked[i].store(0);
        }
    }
}
