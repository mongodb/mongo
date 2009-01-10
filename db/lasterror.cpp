// lasterror.cpp

#include "stdafx.h"
#include "lasterror.h"

boost::thread_specific_ptr<LastError> lastError;
