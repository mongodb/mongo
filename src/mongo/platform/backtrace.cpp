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

#if !defined(_WIN32)
#if defined(__sunos__) || !defined(MONGO_HAVE_EXECINFO_BACKTRACE)

#include "mongo/platform/backtrace.h"

#include <dlfcn.h>

#include "mongo/base/init.h"
#include "mongo/base/status.h"

namespace mongo {
namespace pal {

    int backtrace_emulation(void** array, int size) {
        return 0;
    }

    char** backtrace_symbols_emulation(void* const* array, int size) {
        return NULL;
    }

    void backtrace_symbols_fd_emulation(void* const* array, int size, int fd) {
    }

    typedef int (*BacktraceFunc)(void** array, int size);
    static BacktraceFunc backtrace_switcher =
            pal::backtrace_emulation;

    typedef char** (*BacktraceSymbolsFunc)(void* const* array, int size);
    static BacktraceSymbolsFunc backtrace_symbols_switcher =
            pal::backtrace_symbols_emulation;

    typedef void (*BacktraceSymbolsFdFunc)(void* const* array, int size, int fd);
    static BacktraceSymbolsFdFunc backtrace_symbols_fd_switcher =
            pal::backtrace_symbols_fd_emulation;

    int backtrace(void** array, int size) {
        return backtrace_switcher(array, size);
    }

    char** backtrace_symbols(void* const* array, int size) {
        return backtrace_symbols_switcher(array, size);
    }

    void backtrace_symbols_fd(void* const* array, int size, int fd) {
        backtrace_symbols_fd_switcher(array, size, fd);
    }

} // namespace pal

    // 'backtrace()', 'backtrace_symbols()' and 'backtrace_symbols_fd()' on Solaris will call
    // emulation functions if the symbols are not found
    //
    MONGO_INITIALIZER_GENERAL(SolarisBacktrace,
                              MONGO_NO_PREREQUISITES,
                              ("default"))(InitializerContext* context) {
        void* functionAddress = dlsym(RTLD_DEFAULT, "backtrace");
        if (functionAddress != NULL) {
            pal::backtrace_switcher =
                    reinterpret_cast<pal::BacktraceFunc>(functionAddress);
        }
        functionAddress = dlsym(RTLD_DEFAULT, "backtrace_symbols");
        if (functionAddress != NULL) {
            pal::backtrace_symbols_switcher =
                    reinterpret_cast<pal::BacktraceSymbolsFunc>(functionAddress);
        }
        functionAddress = dlsym(RTLD_DEFAULT, "backtrace_symbols_fd");
        if (functionAddress != NULL) {
            pal::backtrace_symbols_fd_switcher =
                    reinterpret_cast<pal::BacktraceSymbolsFdFunc>(functionAddress);
        }
        return Status::OK();
    }

} // namespace mongo

#endif // #if defined(__sunos__)
#endif // #if !defined(_WIN32)
