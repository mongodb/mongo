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
        Timer W_Timer;

        struct Acquiring {
            Timer tmr;
            LockStat& ls;
            unsigned type;
            explicit Acquiring(LockStat&, char type);
            ~Acquiring();
        };

        void unlocking(char type);

        BSONObj report() const;

    private:
        // RWrw
        AtomicInt64 timeAcquiring[N];
        AtomicInt64 timeLocked[N];

        static unsigned mapNo(char type);
    };

}
