/* concurrency.h

   mongod concurrency rules & notes will be placed here.
*/

#pragma once

namespace mongo {

inline void requireInWriteLock() { 
    assert( dbMutexInfo.isLocked() );
}

}

