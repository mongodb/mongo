// lockstat.h

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
*/


#pragma once

#include "util/timer.h"
#include "mongo/platform/atomic_word.h"

namespace mongo { 

    class BSONObj;

    class LockStat { 
        enum { N = 4 };
    public:
        void recordAcquireTimeMicros( char type , long long micros );
        void recordLockTimeMicros( char type , long long micros );

        void reset();

        BSONObj report() const;
        void report( StringBuilder& builder ) const;

        long long getTimeLocked( char type ) const { return timeLocked[mapNo(type)].load(); }
    private:
        static void _append( BSONObjBuilder& builder, const AtomicInt64* data );
        
        // RWrw
        // in micros
        AtomicInt64 timeAcquiring[N];
        AtomicInt64 timeLocked[N];

        static unsigned mapNo(char type);
        static char nameFor(unsigned offset);
    };

}
