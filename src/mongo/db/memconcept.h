#pragma once

/* The idea here is to 'name' memory pointers so that we can do diagnostics.
   these diagnostics might involve concurrency or other things.  mainly would
   be for _DEBUG builds.  Experimental we'll see how useful.
*/

namespace mongo {
    namespace memconcept {

        /** these are like fancy enums - you can use them as "types" of things 
             and see if foo.concept == bar.concept.
            copyable.
        */
        class concept { 
        public:
            concept() { *this = err; }
            const char * toString() const { return c; }
            static concept err;
            static concept something;
            static concept database;
            static concept other;
            static concept memorymappedfile;
            static concept nsdetails;
            static concept datafileheader;
            static concept extent;
            static concept record;
            static concept deletedrecord;
            static concept btreebucket;
        private:
            const char * c;
            concept(const char *);
        };
        
        /** file was unmapped or something */
        void invalidate(void *p, unsigned len=0);

        /** note you can be more than one thing; a datafile header is also the starting pointer
            for a file */
        void is(void *p, concept c, std::string desc = "", unsigned len=0);

#if 1
//#if !defined(_DEBUG)
        inline void invalidate(void *p, unsigned len) { }
        inline void is(void *p, concept c, std::string, unsigned) { }
#endif

    }
}
