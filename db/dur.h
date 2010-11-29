// @file dur.h durability support

#pragma once

#include "diskloc.h"
#include "mongommf.h"

namespace mongo { 

    namespace dur { 

#if !defined(_DURABLE)
        inline void startup() { }
        inline void* writingPtr(void *x, unsigned len) { return x; }
        inline DiskLoc& writingDiskLoc(DiskLoc& d) { return d; }
        inline int& writingInt(int& d) { return d; }
        template <typename T> inline T* writing(T *x) { return x; }
        inline void assertReading(void *p) { }
        template <typename T> inline T* writingNoLog(T *x) { return x; }
        inline void* writingAtOffset(void *buf, unsigned ofs, unsigned len) { return buf; }
        template <typename T> inline T* alreadyDeclared(T *x) { return x; }
        inline void declareWriteIntent(void *, unsigned) { }
        inline void createdFile(string filename, unsigned long long len) { }
#else

        /** call during startup so durability module can initialize 
            throws if fatal error
        */
        void startup();

        /** Declare that a file has been created 
            Normally writes are applied only after journalling, for safety.  But here the file 
            is created first, and the journal will just replay the creation if the create didn't 
            happen because of crashing.
        */
        void createdFile(string filename, unsigned long long len);

        /** Declarations of write intent.
            
            Use these methods to declare "i'm about to write to x and it should be logged for redo." 
            
            Failure to call writing...() is checked in _DEBUG mode by using a read only mapped view
            (i.e., you'll segfault if the code is covered in that situation).  The _DEBUG check doesn't 
            verify that your length is correct though.
        */

        /** declare intent to write to x for up to len 
            @return pointer where to write.  this is modified when testIntent is true.
        */
        void* writingPtr(void *x, unsigned len);

        /** declare write intent; should already be in the write view to work correctly when testIntent is true. 
            if you aren't, use writingPtr() instead.
        */
        void declareWriteIntent(void *x, unsigned len); 

        /** declare intent to write
            @param ofs offset within buf at which we will write
            @param len the length at ofs we will write
            @return new buffer pointer.  this is modified when testIntent is true.
        */
        void* writingAtOffset(void *buf, unsigned ofs, unsigned len);

        inline DiskLoc& writingDiskLoc(DiskLoc& d) {
            return *((DiskLoc*) writingPtr(&d, sizeof(d)));
        }

        inline int& writingInt(int& d) {
            return *((int*) writingPtr(&d, sizeof(d)));
        }

        /** "assume i've already indicated write intent, let me write"
        */
        template <typename T>
        inline
        T* alreadyDeclared(T *x) { 
#if defined(_TESTINTENT)
            return (T*) MongoMMF::switchToPrivateView(x);
#else
            return x; 
#endif
        }

        /** declare intent to write to x for sizeof(*x) */
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
        inline void assertReading(void *p) { 
            dassert( !testIntent || MongoMMF::switchToPrivateView(p) != p ); 
        }

#endif

    } // namespace dur

    /** declare that we are modifying a diskloc and this is a datafile write. */
    inline DiskLoc& DiskLoc::writing() const { return dur::writingDiskLoc(*const_cast< DiskLoc * >( this )); }

}
