#ifndef PROGRAM_NAME_HPP
#define PROGRAM_NAME_HPP

#include <mutex>
#include <string>

#include "platform/platform.hpp"

#if IS_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#define CPPTRACE_MAX_PATH MAX_PATH

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
    inline const char* program_name() {
        static std::mutex mutex;
        const std::lock_guard<std::mutex> lock(mutex);
        static std::string name;
        static bool did_init = false;
        static bool valid = false;
        if(!did_init) {
            did_init = true;
            char buffer[MAX_PATH + 1];
            int res = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
            if(res) {
                name = buffer;
                valid = true;
            }
        }
        return valid && !name.empty() ? name.c_str() : nullptr;
    }
}
CPPTRACE_END_NAMESPACE

#elif IS_APPLE

#include <cstdint>
#include <mach-o/dyld.h>
#include <sys/syslimits.h>

#define CPPTRACE_MAX_PATH CPPTRACE_PATH_MAX

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
    inline const char* program_name() {
        static std::mutex mutex;
        const std::lock_guard<std::mutex> lock(mutex);
        static std::string name;
        static bool did_init = false;
        static bool valid = false;
        if(!did_init) {
            did_init = true;
            char buffer[CPPTRACE_PATH_MAX + 1];
            std::uint32_t bufferSize = sizeof buffer;
            if(_NSGetExecutablePath(buffer, &bufferSize) == 0) {
                name.assign(buffer, bufferSize);
                valid = true;
            }
        }
        return valid && !name.empty() ? name.c_str() : nullptr;
    }
}
CPPTRACE_END_NAMESPACE

#elif IS_LINUX

#include <sys/types.h>
#include <unistd.h>

#define CPPTRACE_MAX_PATH CPPTRACE_PATH_MAX

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
    inline const char* program_name() {
        static std::mutex mutex;
        const std::lock_guard<std::mutex> lock(mutex);
        static std::string name;
        static bool did_init = false;
        static bool valid = false;
        if(!did_init) {
            did_init = true;
            char buffer[CPPTRACE_PATH_MAX + 1];
            const ssize_t size = readlink("/proc/self/exe", buffer, CPPTRACE_PATH_MAX);
            if(size == -1) {
                return nullptr;
            }
            buffer[size] = 0;
            name = buffer;
            valid = true;
        }
        return valid && !name.empty() ? name.c_str() : nullptr;
    }
}
CPPTRACE_END_NAMESPACE

#endif

#endif
