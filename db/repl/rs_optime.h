// @file rs_optime.h

#pragma once

namespace mongo {

    struct rsoptime { 
        unsigned long long ord;
        void reset() { ord = ~0; }
    };

}
