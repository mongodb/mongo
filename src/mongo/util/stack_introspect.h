// stack_introspect.h

/*    Copyright 2010 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

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
