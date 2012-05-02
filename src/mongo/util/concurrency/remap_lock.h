// remap_lock.h

/*
 *    Copyright 2012 10gen Inc.
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

// Used to synchronize thread creation in initAndListen() in mongod with
// MemoryMappedFile::remapPrivateView() in Windows.  A dummy in mongos.
// The functional constructor & destructor for mongod are in mmap_win.cpp.
// The no-op constructor & destructor for mongos are in server.cpp.

namespace mongo {

    struct RemapLock {
        RemapLock();
        ~RemapLock();
    };

}
