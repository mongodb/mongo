#include "logging.hpp"

#include <cpptrace/forward.hpp>
#include <cpptrace/utils.hpp>

#include <atomic>

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
    std::atomic<log_level> current_log_level(log_level::warning); // NOSONAR

    void default_null_logger(log_level, const char*) {}

    void default_stderr_logger(log_level level, const char* message) {
        microfmt::print(std::cerr, "[cpptrace {}] {}\n", to_string(level), message);
    }

    std::function<void(log_level, const char*)>& log_callback() {
        static std::function<void(log_level, const char*)> callback{default_null_logger};
        return callback;
    }

    void do_log(log_level level, const char* message) {
        if(level < current_log_level) {
            return;
        }
        log_callback()(level, message);
    }

    namespace log {
        void error(const char* message) {
            do_log(log_level::error, message);
        }

        void warn(const char* message) {
            do_log(log_level::warning, message);
        }

        void info(const char* message) {
            do_log(log_level::info, message);
        }

        void debug(const char* message) {
            do_log(log_level::debug, message);
        }
    }
}
CPPTRACE_END_NAMESPACE

CPPTRACE_BEGIN_NAMESPACE
    void set_log_level(log_level level) {
        detail::current_log_level = level;
    }

    void set_log_callback(std::function<void(log_level, const char*)> callback) {
        detail::log_callback() = std::move(callback);
    }

    void use_default_stderr_logger() {
        detail::log_callback() = detail::default_stderr_logger;
    }

    const char* to_string(log_level level) {
        switch(level) {
            case log_level::debug: return "debug";
            case log_level::info: return "info";
            case log_level::warning: return "warning";
            case log_level::error: return "error";
            default: return "unknown log level";
        }
    }
CPPTRACE_END_NAMESPACE
