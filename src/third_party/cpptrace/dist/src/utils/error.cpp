#include "utils/error.hpp"

#include <cpptrace/utils.hpp>

#include <exception>

#include "platform/exception_type.hpp"
#include "logging.hpp"
#include "options.hpp"

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
    internal_error::internal_error(std::string message) : msg("Cpptrace internal error: " + std::move(message)) {}

    constexpr const char* assert_actions[] = {"assertion", "verification", "panic"};
    constexpr const char* assert_names[] = {"ASSERT", "VERIFY", "PANIC"};

    void assert_fail(
        assert_type type,
        const char* expression,
        const char* signature,
        source_location location,
        const char* message
    ) {
        const char* action = assert_actions[static_cast<std::underlying_type<assert_type>::type>(type)];
        const char* name   = assert_names[static_cast<std::underlying_type<assert_type>::type>(type)];
        if(message == nullptr) {
            throw internal_error(
                "Cpptrace {} failed at {}:{}: {}\n"
                "    {}({});\n",
                action, location.file, location.line, signature,
                name, expression
            );
        } else {
            throw internal_error(
                "Cpptrace {} failed at {}:{}: {}: {}\n"
                "    {}({});\n",
                action, location.file, location.line, signature, message,
                name, expression
            );
        }
    }

    void panic(
        const char* signature,
        source_location location,
        string_view message
    ) {
        if(message == "") {
            throw internal_error(
                "Cpptrace panic {}:{}: {}\n",
                location.file, location.line, signature
            );
        } else {
            throw internal_error(
                "Cpptrace panic {}:{}: {}: {}\n",
                location.file, location.line, signature, message
            );
        }
    }

    void log_and_maybe_propagate_exception(std::exception_ptr ptr) {
        try {
            if(ptr) {
                std::rethrow_exception(ptr);
            }
        } catch(const internal_error& e) {
            log::error("Unhandled cpptrace internal error: {}", e.what());
        } catch(const std::exception& e) {
            log::error(
                "Unhandled cpptrace internal error of type {}: {}",
                cpptrace::demangle(typeid(e).name()),
                e.what()
            );
        } catch(...) {
            log::error("Unhandled cpptrace internal error of type {}", detail::exception_type_name());
        }
        if(!should_absorb_trace_exceptions()) {
            if(ptr) {
                std::rethrow_exception(ptr);
            }
        }
    }
}
CPPTRACE_END_NAMESPACE
