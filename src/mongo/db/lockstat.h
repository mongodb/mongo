// lockstat.h
#pragma once

#include "util/timer.h"
#include "mongo/platform/atomic_uint64.h"

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
        AtomicUInt64 timeAcquiring[N];
        AtomicUInt64 timeLocked[N];

        static unsigned mapNo(char type);
    };

}
