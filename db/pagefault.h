// @file pagefault.h

// define this : _PAGEFAULTEXCEPTION

#pragma once

namespace mongo {

    class PageFaultException /*: public DBException*/ { 
        unsigned era;
        Record *r;
    public:
        PageFaultException(const PageFaultException& rhs) : era(rhs.era), r(rhs.r) { }
        PageFaultException(Record*);
        void touch();
    };

    class PageFaultRetryableSection : boost::noncopyable { 
        PageFaultRetryableSection *old;
    public:
        unsigned _laps;
        PageFaultRetryableSection();
        ~PageFaultRetryableSection();
    };

    inline void how_to_use_example() {
        // ...
        {
            PageFaultRetryableSection s;
            while( 1 ) {
                try {
                    writelock lk; // or readlock
                    // do work
                    break;
                }
                catch( PageFaultException& e ) { 
                    e.touch();
                } 
            }
        }
        // ...
    }

}
