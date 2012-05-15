// Copyright 2009.  10gen, Inc.

/**
 * Tools for working with in-process stack traces.
 */

#pragma once

#include <iostream>

namespace mongo {

    // Print stack trace information to "os", default to std::cout.
    void printStackTrace(std::ostream &os=std::cout);

    /**
     * checks up call tree
     * if any method on top of me is a constructor, return true
     * may do internal caching
     * probably slow, use with care
     * if not implemented for a platform, returns false
     */
    bool inConstructorChain( bool printOffending=false );
    
    /**
     * @return if supported on platform, compile options may still prevent it from working
     */
    bool inConstructorChainSupported();

}  // namespace mongo
