#pragma once

namespace mongo {
    namespace lockconcept {

        enum concept {
            err,
            something, 
            database, 
            other,
            memorymappedfile,
            nsdetails,
            datafileheader,
            extent,
            ns,
            record,
            deletedrecord
        }; 
        
        /** file was unmapped or something */
        void invalidate(void *p, unsigned len);
        void invalidate(void *p);

        /** note you can be more than one thing; a datafile header is also the starting pointer
            for a file */
        void is(void *p, concept c);
        void is(void *p, concept c, std::string description);

        inline void invalidate(void *p, unsigned len) { }
        inline void invalidate(void *p) { }
        inline void is(void *p, concept c) { }
        inline void is(void *p, concept c, std::string description) { }

    }
}
