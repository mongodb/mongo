#ifndef LOGGING_HPP
#define LOGGING_HPP

#include <cpptrace/utils.hpp>

#include "utils/microfmt.hpp"

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
namespace log {
    // exported for test purposes
    CPPTRACE_EXPORT void error(const char*);
    template<typename... Args>
    void error(const char* format, Args&&... args) {
        error(microfmt::format(format, args...).c_str());
    }

    CPPTRACE_EXPORT void warn(const char*);
    template<typename... Args>
    void warn(const char* format, Args&&... args) {
        warn(microfmt::format(format, args...).c_str());
    }

    CPPTRACE_EXPORT void info(const char*);
    template<typename... Args>
    void info(const char* format, Args&&... args) {
        info(microfmt::format(format, args...).c_str());
    }

    CPPTRACE_EXPORT void debug(const char*);
    template<typename... Args>
    void debug(const char* format, Args&&... args) {
        debug(microfmt::format(format, args...).c_str());
    }
}
}
CPPTRACE_END_NAMESPACE

#endif
