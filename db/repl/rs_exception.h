// @file rs_exception.h

#pragma once

namespace mongo {

    class VoteException : public std::exception {
    public:
        const char * what() const throw () { return "VoteException"; }
    };

    class RetryAfterSleepException : public std::exception {
    public:
        const char * what() const throw () { return "RetryAfterSleepException"; }
    };

}
