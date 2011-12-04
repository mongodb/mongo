// @file d_concurrency.h

#pragma once

#include "../util/concurrency/rwlock.h"

namespace mongo {

    class HLock { 
    public:
        HLock(int level, HLock *parent, RWLock& r);
        struct writelock { 
            writelock(HLock&);
            ~writelock();
        private: 
            HLock& h;
            bool already;
        };
        struct readlock { 
            readlock(HLock&); 
            ~readlock();
        private:
            HLock& h; int nToLock;
        };
        struct TLS { 
            TLS(); ~TLS(); int x;
        };
    private:
        void hlock();
        void hunlock();
        void hlockShared(int n);
        void hunlockShared(int n);
        HLock * const parent;
        RWLock& r;
        const int level; // 1=global, 2=db, 3=collection
    };

    // CLC turns on the "collection level concurrency" code which is under development
    // and not finished.
#if defined(CLC)
    ...
#endif

}
