#ifndef ERROR_HPP
#define ERROR_HPP

#include <exception>
#include <string>
#include <utility>

#include "platform/platform.hpp"
#include "utils/microfmt.hpp"
#include "utils/string_view.hpp"

#include <cpptrace/utils.hpp>

#if IS_MSVC
 #define CPPTRACE_PFUNC __FUNCSIG__
#else
 #define CPPTRACE_PFUNC __extension__ __PRETTY_FUNCTION__
#endif

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
    class internal_error : public std::exception {
        std::string msg;
    public:
        internal_error(std::string message);
        template<typename... Args>
        internal_error(const char* format, Args&&... args) : internal_error(microfmt::format(format, args...)) {}
        const char* what() const noexcept override {
            return msg.c_str();
        }
    };

    // Lightweight std::source_location.
    struct source_location {
        const char* const file;
        const int line;
        constexpr source_location(
            const char* _file,
            int _line
        ) : file(_file), line(_line) {}
    };

    #define CPPTRACE_CURRENT_LOCATION ::cpptrace::detail::source_location(__FILE__, __LINE__)

    enum class assert_type {
        assert,
        verify,
        panic,
    };

    [[noreturn]] CPPTRACE_EXPORT void assert_fail(
        assert_type type,
        const char* expression,
        const char* signature,
        source_location location,
        const char* message
    );

    [[noreturn]] void panic(
        const char* signature,
        source_location location,
        string_view message = ""
    );


    template<typename... Args>
    void nullfn(Args&&...);

    #define PHONY_USE(...) (static_cast<decltype(nullfn(__VA_ARGS__))>(0))

    // Work around a compiler warning
    template<typename T>
    std::string as_string(T&& value) {
        return std::string(std::forward<T>(value));
    }

    inline std::string as_string() {
        return "";
    }

    // Check condition in both debug and release. std::runtime_error on failure.
    #define PANIC(...) ((::cpptrace::detail::panic)(CPPTRACE_PFUNC, CPPTRACE_CURRENT_LOCATION, ::cpptrace::detail::as_string(__VA_ARGS__)))

    inline void assert_impl(
        bool condition,
        const char* message,
        assert_type type,
        const char* args,
        const char* signature,
        source_location location
    ) {
        if(!condition) {
            assert_fail(type, args, signature, location, message);
        }
    }

    inline void assert_impl(
        bool condition,
        assert_type type,
        const char* args,
        const char* signature,
        source_location location
    ) {
        assert_impl(
            condition,
            nullptr,
            type,
            args,
            signature,
            location
        );
    }

    // Check condition in both debug and release. std::runtime_error on failure.
    #define VERIFY(...) ( \
        assert_impl(__VA_ARGS__, ::cpptrace::detail::assert_type::verify, #__VA_ARGS__, CPPTRACE_PFUNC, CPPTRACE_CURRENT_LOCATION) \
    )

    #ifndef NDEBUG
     // Check condition in both debug. std::runtime_error on failure.
     #define ASSERT(...) ( \
        assert_impl(__VA_ARGS__, ::cpptrace::detail::assert_type::assert, #__VA_ARGS__, CPPTRACE_PFUNC, CPPTRACE_CURRENT_LOCATION) \
     )
    #else
     // Check condition in both debug. std::runtime_error on failure.
     #define ASSERT(...) PHONY_USE(__VA_ARGS__)
    #endif

    void log_and_maybe_propagate_exception(std::exception_ptr);
}
CPPTRACE_END_NAMESPACE

#endif
