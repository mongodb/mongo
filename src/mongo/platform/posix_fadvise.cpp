/*    Copyright 2013 10gen Inc.
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

#if defined(__sunos__)

#include "mongo/platform/posix_fadvise.h"

#include <dlfcn.h>

#include "mongo/base/init.h"
#include "mongo/base/status.h"

namespace mongo {
namespace pal {

    int posix_fadvise_emulation(int fd, off_t offset, off_t len, int advice) {
        return 0;
    }

    typedef int (*PosixFadviseFunc)(int fd, off_t offset, off_t len, int advice);
    static PosixFadviseFunc posix_fadvise_switcher = mongo::pal::posix_fadvise_emulation;

    int posix_fadvise(int fd, off_t offset, off_t len, int advice) {
        return posix_fadvise_switcher(fd, offset, len, advice);
    }

} // namespace pal

    // 'posix_fadvise()' on Solaris will call the emulation if the symbol is not found
    //
    MONGO_INITIALIZER_GENERAL(SolarisPosixFadvise,
                              MONGO_NO_PREREQUISITES,
                              ("default"))(InitializerContext* context) {
        void* functionAddress = dlsym(RTLD_DEFAULT, "posix_fadvise");
        if (functionAddress != NULL) {
            mongo::pal::posix_fadvise_switcher =
                    reinterpret_cast<mongo::pal::PosixFadviseFunc>(functionAddress);
        }
        return Status::OK();
    }

} // namespace mongo

#endif // #if defined(__sunos__)
