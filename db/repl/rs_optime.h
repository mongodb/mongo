// @file rs_optime.h

#pragma once

namespace mongo {

    struct rsoptime { 
        unsigned long long ord;

        rsoptime() : ord(0) { }

        bool initiated() const { return ord > 0; }

        void initiate() { 
            assert( !initiated() );
            ord = 1000000;
        }
    };

    extern rsoptime rsOpTime;

}
