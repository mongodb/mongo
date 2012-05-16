// stack_introspect.h

#pragma once

namespace mongo {

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

}
