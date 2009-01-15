// lasterror.cpp

#include "stdafx.h"
#include "lasterror.h"

namespace mongo {

    boost::thread_specific_ptr<LastError> lastError;

} // namespace mongo
