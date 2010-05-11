// @file rstime.h

#pragma once

namespace mongo {

    struct rstime { 
        unsigned long long v;
        unsigned server() const { return (unsigned) (v >> (64-8)); }
        unsigned long long op() const { return v & 0xffffffffffffffLL; }
    };

}
