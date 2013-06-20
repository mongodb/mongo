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
#if defined(__sunos__) || !defined(MONGO_HAVE_EXECINFO_BACKTRACE)

namespace mongo {
    namespace pal {
        int backtrace(void** array, int size);
        char** backtrace_symbols(void* const* array, int size);
        void backtrace_symbols_fd(void* const* array, int size, int fd);
    } // namespace pal
    using pal::backtrace;
    using pal::backtrace_symbols;
    using pal::backtrace_symbols_fd;
} // namespace mongo

#else

#include <execinfo.h>

namespace mongo {
    using ::backtrace;
    using ::backtrace_symbols;
    using ::backtrace_symbols_fd;
} // namespace mongo

#endif
#endif
