// @file rs_exception.h

#pragma once

namespace mongo { 

    class VoteException : public std::exception { };

    class RetryAfterSleepException : public std::exception { };

}



