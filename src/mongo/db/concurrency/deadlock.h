// deadlock.h

#pragma once

#include <exception>

namespace mongo {

    class DeadLockException : public std::exception {
    public:
        virtual const char* what() const throw() { return "DeadLock"; }
    };

}
