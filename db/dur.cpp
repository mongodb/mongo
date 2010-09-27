// @file dur.cpp

#include "pch.h"
#include "dur.h"
#include "../util/mmap.h"

namespace mongo { 

    namespace dur { 

#if defined(_DEBUG) && defined(_DURABLE)

        void* writingPtr(void *x, size_t len) { 
            cout << "TEMP writing " << x << ' ' << len << endl;
            return MemoryMappedFile::getWriteViewFor(x);
        }

        void assertReading(void *p) { 
            assert( MemoryMappedFile::getWriteViewFor(p) != 
                    p );
        }
        void assertWriting(void *p) {
            // todo: 
            //assert( MemoryMappedFile::getWriteViewFor(p) == 
            //        p );
       }

#endif

    }

}
