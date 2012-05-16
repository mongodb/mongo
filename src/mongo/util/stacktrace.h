// Copyright 2009.  10gen, Inc.

/**
 * Tools for working with in-process stack traces.
 */

#pragma once

#include <iostream>

namespace mongo {

    // Print stack trace information to "os", default to std::cout.
    void printStackTrace(std::ostream &os=std::cout);

}  // namespace mongo
