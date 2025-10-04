#ifndef CPPTRACE_EXCEPTIONS_MACROS_HPP
#define CPPTRACE_EXCEPTIONS_MACROS_HPP

// Exception wrapper utilities
#define CPPTRACE_WRAP_BLOCK(statements) do { \
        try { \
            statements \
        } catch(...) { \
            ::cpptrace::rethrow_and_wrap_if_needed(); \
        } \
    } while(0)

#define CPPTRACE_WRAP(expression) [&] () -> decltype((expression)) { \
        try { \
            return expression; \
        } catch(...) { \
            ::cpptrace::rethrow_and_wrap_if_needed(1); \
        } \
    } ()

#endif // CPPTRACE_EXCEPTIONS_MACROS_HPP
