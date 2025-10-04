#include <cpptrace/exceptions.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <new>
#include <stdexcept>
#include <string>

#include "platform/exception_type.hpp"
#include "utils/common.hpp"
#include "options.hpp"
#include "logging.hpp"
#include "utils/error.hpp"

CPPTRACE_BEGIN_NAMESPACE
    namespace detail {
        lazy_trace_holder::lazy_trace_holder(const lazy_trace_holder& other) : resolved(other.resolved) {
            if(other.resolved) {
                new (&resolved_trace) stacktrace(other.resolved_trace);
            } else {
                new (&trace) raw_trace(other.trace);
            }
        }
        lazy_trace_holder::lazy_trace_holder(lazy_trace_holder&& other) noexcept : resolved(other.resolved) {
            if(other.resolved) {
                new (&resolved_trace) stacktrace(std::move(other.resolved_trace));
            } else {
                new (&trace) raw_trace(std::move(other.trace));
            }
        }
        lazy_trace_holder& lazy_trace_holder::operator=(const lazy_trace_holder& other) {
            clear();
            resolved = other.resolved;
            if(other.resolved) {
                new (&resolved_trace) stacktrace(other.resolved_trace);
            } else {
                new (&trace) raw_trace(other.trace);
            }
            return *this;
        }
        lazy_trace_holder& lazy_trace_holder::operator=(lazy_trace_holder&& other) noexcept {
            clear();
            resolved = other.resolved;
            if(other.resolved) {
                new (&resolved_trace) stacktrace(std::move(other.resolved_trace));
            } else {
                new (&trace) raw_trace(std::move(other.trace));
            }
            return *this;
        }
        lazy_trace_holder::~lazy_trace_holder() {
            clear();
        }
        // access
        const raw_trace& lazy_trace_holder::get_raw_trace() const {
            if(resolved) {
                throw std::logic_error(
                    "cpptrace::detail::lazy_trace_holder::get_resolved_trace called on resolved holder"
                );
            }
            return trace;
        }
        stacktrace& lazy_trace_holder::get_resolved_trace() {
            if(!resolved) {
                raw_trace old_trace = std::move(trace);
                *this = lazy_trace_holder(stacktrace{});
                try {
                    if(!old_trace.empty()) {
                        resolved_trace = old_trace.resolve();
                    }
                } catch(const std::exception& e) {
                    if(!should_absorb_trace_exceptions()) {
                        log::error(
                            "Exception occurred while resolving trace in cpptrace::detail::lazy_trace_holder: {}",
                            e.what()
                        );
                    }
                }
            }
            return resolved_trace;
        }
        const stacktrace& lazy_trace_holder::get_resolved_trace() const {
            if(!resolved) {
                throw std::logic_error(
                    "cpptrace::detail::lazy_trace_holder::get_resolved_trace called on unresolved const holder"
                );
            }
            return resolved_trace;
        }
        bool lazy_trace_holder::is_resolved() const {
            return resolved;
        }
        void lazy_trace_holder::clear() {
            if(resolved) {
                resolved_trace.~stacktrace();
            } else {
                trace.~raw_trace();
            }
        }

        CPPTRACE_FORCE_NO_INLINE
        raw_trace get_raw_trace_and_absorb(std::size_t skip, std::size_t max_depth) {
            try {
                return generate_raw_trace(skip + 1, max_depth);
            } catch(const std::exception& e) {
                if(!should_absorb_trace_exceptions()) {
                    log::error(
                        "Exception occurred while resolving trace in cpptrace::exception object: {}",
                        e.what()
                    );
                }
                return raw_trace{};
            }
        }

        CPPTRACE_FORCE_NO_INLINE
        raw_trace get_raw_trace_and_absorb(std::size_t skip) {
            try { // try/catch can never be hit but it's needed to prevent TCO
                return get_raw_trace_and_absorb(skip + 1, SIZE_MAX);
            } catch(...) {
                detail::log_and_maybe_propagate_exception(std::current_exception());
                return raw_trace{};
            }
        }
    }

    const char* lazy_exception::what() const noexcept {
        if(what_string.empty()) {
            what_string = message() + std::string(":\n") + trace_holder.get_resolved_trace().to_string();
        }
        return what_string.c_str();
    }

    const char* lazy_exception::message() const noexcept {
        return "cpptrace::lazy_exception";
    }

    const stacktrace& lazy_exception::trace() const noexcept {
        return trace_holder.get_resolved_trace();
    }

    const char* exception_with_message::message() const noexcept {
        return user_message.c_str();
    }

    system_error::system_error(int error_code, std::string&& message_arg, raw_trace&& trace) noexcept
        : runtime_error(
            message_arg + ": " + std::error_code(error_code, std::generic_category()).message(),
            std::move(trace)
          ),
          ec(std::error_code(error_code, std::generic_category())) {}

    const std::error_code& system_error::code() const noexcept {
        return ec;
    }

    const char* nested_exception::message() const noexcept {
        if(message_value.empty()) {
            try {
                std::rethrow_exception(ptr);
            } catch(std::exception& e) {
                message_value = std::string("Nested exception: ") + e.what();
            } catch(...) {
                message_value = "Nested exception holding instance of " + detail::exception_type_name();
            }
        }
        return message_value.c_str();
    }

    std::exception_ptr nested_exception::nested_ptr() const noexcept {
        return ptr;
    }

    CPPTRACE_FORCE_NO_INLINE
    void rethrow_and_wrap_if_needed(std::size_t skip) {
        try {
            std::rethrow_exception(std::current_exception());
        } catch(cpptrace::exception&) {
            throw; // already a cpptrace::exception
        } catch(...) {
            throw nested_exception(std::current_exception(), detail::get_raw_trace_and_absorb(skip + 1));
        }
    }
CPPTRACE_END_NAMESPACE
