// @file rstime.h

#pragma once

namespace mongo {

    struct rstime { 
        unsigned long long v;
        unsigned server() const { return v >> (64-8); }
        unsigned op() const { return v & 0xffffffffffffffL; }
    };

}