// @file dur.h durability support

#pragma once

#include "diskloc.h"
#include "mongommf.h"

namespace mongo { 

    namespace dur { 

#if !defined(_DURABLE)
        inline void startup() { }
        inline void* writingPtr(void *x, size_t len) { return x; }
        inline DiskLoc& writingDiskLoc(DiskLoc& d) { return d; }
        inline int& writingInt(int& d) { return d; }
        template <typename T> inline T* writing(T *x) { return x; }
        inline void assertReading(void *p) { }
        template <typename T> inline T* writingNoLog(T *x) { return x; }
#else

        /** call during startup so durability module can initialize */
        void startup();

        /** Declarations of write intent.
            
            Use these methods to declare "i'm about to write to x and it should be logged for redo." 
            
            Failure to call writing...() is checked in _DEBUG mode by using a read only mapped view
            (i.e., you'll segfault if the code is covered in that situation).  The _DEBUG check doesn't 
            verify that your length is correct though.
        */

        void* writingPtr(void *x, size_t len);

        inline DiskLoc& writingDiskLoc(DiskLoc& d) {
            return *((DiskLoc*) writingPtr(&d, sizeof(d)));
        }

        inline int& writingInt(int& d) {
            return *((int*) writingPtr(&d, sizeof(d)));
        }

        template <typename T> 
        inline 
        T* writing(T *x) { 
            return (T*) writingPtr(x, sizeof(T));
        }

        /** declare our intent to write, but it doesn't have to be journaled, as this write is 
            something 'unimportant'.  depending on our implementation, we may or may not be able 
            to take advantage of this versus doing the normal work we do.
        */
        template <typename T> 
        inline 
        T* writingNoLog(T *x) { 
            DEV RARELY log() << "todo dur nolog not yet optimized" << endl;
            return (T*) writingPtr(x, sizeof(T));
        }

        /* assert that we have not (at least so far) declared write intent for p */
        inline void assertReading(void *p) { dassert( MongoMMF::switchToPrivateView(p) != p ); }

#endif

    } // namespace dur

    inline DiskLoc& DiskLoc::writing() const { return dur::writingDiskLoc(*const_cast< DiskLoc * >( this )); }

}
