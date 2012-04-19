#pragma once

#include "util/timer.h"

namespace mongo { 

    class BSONObj;

    class LockStat { 
        enum { N = 4 };
    public:
        Timer W_Timer;
        unsigned long long timeAcquiring[N];
        unsigned long long timeLocked[N];

        LockStat() { 
            for( int i = 0; i < N; i++ ) {
                timeAcquiring[i] = 0;
                timeLocked[i] = 0;
            }
        }

        struct Acquiring {
            Timer tmr;
            LockStat& ls;
            unsigned type;
            explicit Acquiring(LockStat&, char type);
            ~Acquiring();
        };

        void unlocking(char type);

        BSONObj report() const;

        static unsigned map(char type);
    };

}
