// Copyright 2009.  10gen, Inc.

/**
 * Tools for working with in-process stack traces.
 */

#pragma once

#include <iostream>

#include "mongo/platform/basic.h"

namespace mongo {

    // Print stack trace information to "os", default to std::cout.
    void printStackTrace(std::ostream &os=std::cout);

#if defined(_WIN32)
    // Print stack trace (using a specified stack context) to "os", default to std::cout.
    void printWindowsStackTrace(CONTEXT &context, std::ostream &os=std::cout);
#endif

}  // namespace mongo
