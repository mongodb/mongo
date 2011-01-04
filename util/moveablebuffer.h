/* moveablebuffer.h
*/

/*    Copyright 2009 10gen Inc.
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

    /** this is a sort of smart pointer class where we can move where something is and all the pointers will adjust.
        not threadsafe.
        */
    struct MoveableBuffer {
        MoveableBuffer();
        MoveableBuffer(void *);
        MoveableBuffer& operator=(const MoveableBuffer&);
        ~MoveableBuffer();

        void *p;
    };

    /* implementation (inlines) below */

    // this is a temp stub implementation...not really done yet - just having everything compile & such for checkpointing into git

    inline MoveableBuffer::MoveableBuffer() : p(0) { }

    inline MoveableBuffer::MoveableBuffer(void *_p) : p(_p) { }

    inline MoveableBuffer& MoveableBuffer::operator=(const MoveableBuffer& r) {
        p = r.p;
        return *this;
    }

    inline MoveableBuffer::~MoveableBuffer() {
    }

}
