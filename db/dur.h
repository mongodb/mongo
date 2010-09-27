// @file dur.h durability support

#pragma once

#include "diskloc.h"

namespace mongo { 

    namespace dur { 

        /** call writing...() to declare "i'm about to write to x and it should be logged for redo." 
            
            failure to call writing...() is checked in _DEBUG mode by using a read only mapped view
            (i.e., you'll segfault if you don't...)
        */


#if !defined(_DURABLE)

        inline void* writingPtr(void *x, size_t len) { return x; }
        inline DiskLoc& writingDiskLoc(DiskLoc& d) { return d; }
        inline int& writingInt(int& d) { return d; }
        template <typename T> inline T* writing(T *x) { return x; }
        inline void assertReading(void *p) { }
        inline void assertWriting(void *p) { }

#else

        void* writingPtr(void *x, size_t len);

        inline DiskLoc& writingDiskLoc(DiskLoc& d) {
#if defined(_DEBUG)
            return *((DiskLoc*) writingPtr(&d, sizeof(d)));
#else
            return d;
#endif
        }

        inline int& writingInt(int& d) {
#if defined(_DEBUG)
            return *((int*) writingPtr(&d, sizeof(d)));
#else
            return d;
#endif
        }

        template <typename T> 
        inline 
        T* writing(T *x) { 
#if defined(_DEBUG)
            return (T*) writingPtr(x, sizeof(T));
#else
            return x;
#endif
        }

        void assertReading(void *p);
        void assertWriting(void *p);

#endif

    }

    inline DiskLoc& DiskLoc::writing() { 
        return dur::writingDiskLoc(*this);
    }

}
