#ifndef CPPTRACE_EXCEPTIONS_HPP
#define CPPTRACE_EXCEPTIONS_HPP

#include <cpptrace/basic.hpp>
#include <cpptrace/exceptions_macros.hpp>

#include <exception>
#include <system_error>

#ifdef _MSC_VER
#pragma warning(push)
// warning C4251: using non-dll-exported type in dll-exported type, firing on std::vector<frame_ptr> and others for some
// reason
// 4275 is the same thing but for base classes
#pragma warning(disable: 4251; disable: 4275)
#endif

CPPTRACE_BEGIN_NAMESPACE
    // tracing exceptions:
    namespace detail {
        // This is a helper utility, if the library weren't C++11 an std::variant would be used
        class CPPTRACE_EXPORT lazy_trace_holder {
            bool resolved;
            union {
                raw_trace trace;
                stacktrace resolved_trace;
            };
        public:
            // constructors
            lazy_trace_holder() : resolved(false), trace() {}
            explicit lazy_trace_holder(raw_trace&& _trace) : resolved(false), trace(std::move(_trace)) {}
            explicit lazy_trace_holder(stacktrace&& _resolved_trace) : resolved(true), resolved_trace(std::move(_resolved_trace)) {}
            // logistics
            lazy_trace_holder(const lazy_trace_holder& other);
            lazy_trace_holder(lazy_trace_holder&& other) noexcept;
            lazy_trace_holder& operator=(const lazy_trace_holder& other);
            lazy_trace_holder& operator=(lazy_trace_holder&& other) noexcept;
            ~lazy_trace_holder();
            // access
            const raw_trace& get_raw_trace() const;
            stacktrace& get_resolved_trace();
            const stacktrace& get_resolved_trace() const;
            bool is_resolved() const;
        private:
            void clear();
        };

        CPPTRACE_EXPORT raw_trace get_raw_trace_and_absorb(std::size_t skip, std::size_t max_depth);
        CPPTRACE_EXPORT raw_trace get_raw_trace_and_absorb(std::size_t skip = 0);
    }

    // Interface for a traced exception object
    class CPPTRACE_EXPORT exception : public std::exception {
    public:
        const char* what() const noexcept override = 0;
        virtual const char* message() const noexcept = 0;
        virtual const stacktrace& trace() const noexcept = 0;
    };

    // Cpptrace traced exception object
    // I hate to have to expose anything about implementation detail but the idea here is that
    class CPPTRACE_EXPORT lazy_exception : public exception {
        mutable detail::lazy_trace_holder trace_holder;
        mutable std::string what_string;

    public:
        explicit lazy_exception(
            raw_trace&& trace = detail::get_raw_trace_and_absorb()
        ) : trace_holder(std::move(trace)) {}
        // std::exception
        const char* what() const noexcept override;
        // cpptrace::exception
        const char* message() const noexcept override;
        const stacktrace& trace() const noexcept override;
    };

    class CPPTRACE_EXPORT exception_with_message : public lazy_exception {
        mutable std::string user_message;

    public:
        explicit exception_with_message(
            std::string&& message_arg,
            raw_trace&& trace = detail::get_raw_trace_and_absorb()
        ) noexcept : lazy_exception(std::move(trace)), user_message(std::move(message_arg)) {}

        const char* message() const noexcept override;
    };

    class CPPTRACE_EXPORT logic_error : public exception_with_message {
    public:
        explicit logic_error(
            std::string&& message_arg,
            raw_trace&& trace = detail::get_raw_trace_and_absorb()
        ) noexcept
            : exception_with_message(std::move(message_arg), std::move(trace)) {}
    };

    class CPPTRACE_EXPORT domain_error : public exception_with_message {
    public:
        explicit domain_error(
            std::string&& message_arg,
            raw_trace&& trace = detail::get_raw_trace_and_absorb()
        ) noexcept
            : exception_with_message(std::move(message_arg), std::move(trace)) {}
    };

    class CPPTRACE_EXPORT invalid_argument : public exception_with_message {
    public:
        explicit invalid_argument(
            std::string&& message_arg,
            raw_trace&& trace = detail::get_raw_trace_and_absorb()
        ) noexcept
            : exception_with_message(std::move(message_arg), std::move(trace)) {}
    };

    class CPPTRACE_EXPORT length_error : public exception_with_message {
    public:
        explicit length_error(
            std::string&& message_arg,
            raw_trace&& trace = detail::get_raw_trace_and_absorb()
        ) noexcept
            : exception_with_message(std::move(message_arg), std::move(trace)) {}
    };

    class CPPTRACE_EXPORT out_of_range : public exception_with_message {
    public:
        explicit out_of_range(
            std::string&& message_arg,
            raw_trace&& trace = detail::get_raw_trace_and_absorb()
        ) noexcept
            : exception_with_message(std::move(message_arg), std::move(trace)) {}
    };

    class CPPTRACE_EXPORT runtime_error : public exception_with_message {
    public:
        explicit runtime_error(
            std::string&& message_arg,
            raw_trace&& trace = detail::get_raw_trace_and_absorb()
        ) noexcept
            : exception_with_message(std::move(message_arg), std::move(trace)) {}
    };

    class CPPTRACE_EXPORT range_error : public exception_with_message {
    public:
        explicit range_error(
            std::string&& message_arg,
            raw_trace&& trace = detail::get_raw_trace_and_absorb()
        ) noexcept
            : exception_with_message(std::move(message_arg), std::move(trace)) {}
    };

    class CPPTRACE_EXPORT overflow_error : public exception_with_message {
    public:
        explicit overflow_error(
            std::string&& message_arg,
            raw_trace&& trace = detail::get_raw_trace_and_absorb()
        ) noexcept
            : exception_with_message(std::move(message_arg), std::move(trace)) {}
    };

    class CPPTRACE_EXPORT underflow_error : public exception_with_message {
    public:
        explicit underflow_error(
            std::string&& message_arg,
            raw_trace&& trace = detail::get_raw_trace_and_absorb()
        ) noexcept
            : exception_with_message(std::move(message_arg), std::move(trace)) {}
    };

    class CPPTRACE_EXPORT nested_exception : public lazy_exception {
        std::exception_ptr ptr;
        mutable std::string message_value;
    public:
        explicit nested_exception(
            const std::exception_ptr& exception_ptr,
            raw_trace&& trace = detail::get_raw_trace_and_absorb()
        ) noexcept
            : lazy_exception(std::move(trace)), ptr(exception_ptr) {}

        const char* message() const noexcept override;
        std::exception_ptr nested_ptr() const noexcept;
    };

    class CPPTRACE_EXPORT system_error : public runtime_error {
        std::error_code ec;
    public:
        explicit system_error(
            int error_code,
            std::string&& message_arg,
            raw_trace&& trace = detail::get_raw_trace_and_absorb()
        ) noexcept;
        const std::error_code& code() const noexcept;
    };

    // [[noreturn]] must come first due to old clang
    [[noreturn]] CPPTRACE_EXPORT void rethrow_and_wrap_if_needed(std::size_t skip = 0);
CPPTRACE_END_NAMESPACE

#ifdef _MSC_VER
#pragma warning(pop)
#endif

#endif
