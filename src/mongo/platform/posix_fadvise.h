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

#pragma once

#if !defined(_WIN32)

#include <fcntl.h>

#if defined(__sunos__)

#include <sys/types.h>

namespace mongo {
    namespace pal {
        int posix_fadvise(int fd, off_t offset, off_t len, int advice);
    } // namespace pal
    using pal::posix_fadvise;
} // namespace mongo

#elif defined(POSIX_FADV_DONTNEED)

namespace mongo {
    using ::posix_fadvise;
} // namespace mongo

#endif

#endif
