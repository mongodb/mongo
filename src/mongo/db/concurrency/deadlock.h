// deadlock.h

#pragma once

#include <exception>

#include "mongo/util/assert_util.h"

namespace mongo {

    class DeadLockException : public DBException {
    public:
        DeadLockException() : DBException( "deadlock", ErrorCodes::DeadLock ){}
    };

}
