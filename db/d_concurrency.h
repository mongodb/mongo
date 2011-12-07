// @file d_concurrency.h

#pragma once

#include "../util/concurrency/rwlock.h"

#if defined(CLC)

#error asdf

namespace mongo {

    class HLock { 
    public:
        HLock(HLock *parent, RWLock& r);
        struct writelock { 
            writelock(HLock&);
            ~writelock();
        private: 
            HLock& h;
            bool already;
        };
        struct readlock { 
            readlock(HLock&);
        private:
            HLock& h;
            bool already;
        };
    private:
        void hlock();
        void hunlock();
        void hlockShared();
        void hunlockShared();
        HLock *parent;
        RWLock& r;
    };

}

#endif
