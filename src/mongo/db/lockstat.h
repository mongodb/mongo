#include "util/timer.h"

namespace mongo { 

    class LockStat { 
        enum { N = 4 };
    public:
        Timer t;
        unsigned long long timeAcquiring[N];
        unsigned long long timeLocked[N];

        LockStat() { 
            for( int i = 0; i < N; i++ ) {
                timeAcquiring[i] = 0;
                timeLocked[i] = 0;
            }
        }

        struct Acquiring {
            LockStat& ls;
            unsigned type;
            explicit Acquiring(LockStat&, char type);
            ~Acquiring();
        };

        void unlocking(char type);

        BSONObj report() const;

        static unsigned map(char type);
    };

    inline BSONObj LockStat::report() const { 
        return BSON(
            "timeLocked" << 
               BSON(
                 "R" << (long long) timeLocked[0] << 
                 "W" << (long long) timeLocked[1] << 
                 "w" << (long long) timeLocked[2] << 
                 "r" << (long long) timeLocked[3]) << 
            "timeAcquiring" << 
               BSON(
                 "R" << (long long) timeAcquiring[0] << 
                 "W" << (long long) timeAcquiring[1] << 
                 "w" << (long long) timeAcquiring[2] << 
                 "r" << (long long) timeAcquiring[3])
        );
    }

    inline unsigned LockStat::map(char type) {
        switch( type ) { 
        case 'R' : return 0;
        case 'W' : return 1;
        case 'r' : return 2;
        case 'w' : return 3;
        default: ;
        }
        fassert(0,false);
        return 0;
    }

    inline LockStat::Acquiring::Acquiring(LockStat& _ls, char t) : ls(_ls) { 
        type = map(t);
        dassert( type < N );
        ls.t.reset();
    }

    // note: we have race conditions on the following += 
    // hmmm....

    inline LockStat::Acquiring::~Acquiring() { 
        ls.timeAcquiring[type] += ls.t.microsReset(); // reset to time how long we stay locked
    }

    inline void LockStat::unlocking(char tp) { 
        unsigned type = map(tp);
        timeLocked[type] += t.micros();
    }
}
