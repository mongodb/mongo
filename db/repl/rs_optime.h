// @file rs_optime.h

#pragma once

namespace mongo {

    struct RSOpTime { 
        unsigned long long ord;

        RSOpTime() : ord(0) { }

        bool initiated() const { return ord > 0; }

        void initiate() { 
            assert( !initiated() );
            ord = 1000000;
        }

        ReplTime inc() {
            DEV assertInWriteLock();
            return ++ord;
        }
    };

    extern RSOpTime rsOpTime;

}
