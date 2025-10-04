#ifndef CPPTRACE_IO_HPP
#define CPPTRACE_IO_HPP

#include <cpptrace/basic.hpp>

#include <iosfwd>

#ifndef CPPTRACE_NO_STD_FORMAT
 #if __cplusplus >= 202002L
  #ifdef __has_include
   #if __has_include(<format>)
    #define CPPTRACE_STD_FORMAT
    #include <format>
   #endif
  #endif
 #endif
#endif

#ifdef _MSC_VER
#pragma warning(push)
// warning C4251: using non-dll-exported type in dll-exported type, firing on std::vector<frame_ptr> and others for some
// reason
// 4275 is the same thing but for base classes
#pragma warning(disable: 4251; disable: 4275)
#endif

CPPTRACE_BEGIN_NAMESPACE
    CPPTRACE_EXPORT std::ostream& operator<<(std::ostream& stream, const stacktrace_frame& frame);
    CPPTRACE_EXPORT std::ostream& operator<<(std::ostream& stream, const stacktrace& trace);
CPPTRACE_END_NAMESPACE

#if defined(CPPTRACE_STD_FORMAT) && defined(__cpp_lib_format)
 template <>
 struct std::formatter<cpptrace::stacktrace_frame> : std::formatter<std::string> {
     auto format(cpptrace::stacktrace_frame frame, format_context& ctx) const {
         return formatter<string>::format(frame.to_string(), ctx);
     }
 };

 template <>
 struct std::formatter<cpptrace::stacktrace> : std::formatter<std::string> {
     auto format(cpptrace::stacktrace trace, format_context& ctx) const {
         return formatter<string>::format(trace.to_string(), ctx);
     }
 };
#endif

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#endif
