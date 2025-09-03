#ifndef CPPTRACE_UTILS_HPP
#define CPPTRACE_UTILS_HPP

#include <cpptrace/basic.hpp>

#include <functional>

#ifdef _MSC_VER
#pragma warning(push)
// warning C4251: using non-dll-exported type in dll-exported type, firing on std::vector<frame_ptr> and others for some
// reason
// 4275 is the same thing but for base classes
#pragma warning(disable: 4251; disable: 4275)
#endif

CPPTRACE_BEGIN_NAMESPACE
    CPPTRACE_EXPORT std::string demangle(const std::string& name);

    CPPTRACE_EXPORT std::string basename(const std::string& path);
    CPPTRACE_EXPORT std::string prettify_symbol(std::string symbol);
    CPPTRACE_EXPORT std::string prune_symbol(const std::string& symbol);

    CPPTRACE_EXPORT std::string get_snippet(
        const std::string& path,
        std::size_t line,
        std::size_t context_size,
        bool color = false
    );

    CPPTRACE_EXPORT std::string get_snippet(
        const std::string& path,
        std::size_t line,
        nullable<std::uint32_t> column,
        std::size_t context_size,
        bool color = false
    );

    CPPTRACE_EXPORT bool isatty(int fd);

    CPPTRACE_EXPORT extern const int stdin_fileno;
    CPPTRACE_EXPORT extern const int stderr_fileno;
    CPPTRACE_EXPORT extern const int stdout_fileno;

    CPPTRACE_EXPORT void register_terminate_handler();

    // options:
    CPPTRACE_EXPORT void absorb_trace_exceptions(bool absorb);
    CPPTRACE_EXPORT void enable_inlined_call_resolution(bool enable);

    enum class log_level { debug, info, warning, error };
    CPPTRACE_EXPORT void set_log_level(log_level level);
    CPPTRACE_EXPORT void set_log_callback(std::function<void(log_level, const char*)>);
    CPPTRACE_EXPORT void use_default_stderr_logger();
    CPPTRACE_EXPORT const char* to_string(log_level level);

    enum class cache_mode {
        // Only minimal lookup tables
        prioritize_memory = 0,
        // Build lookup tables but don't keep them around between trace calls
        hybrid = 1,
        // Build lookup tables as needed
        prioritize_speed = 2
    };

    namespace experimental {
        CPPTRACE_EXPORT void set_cache_mode(cache_mode mode);
    }

    // dwarf options
    namespace experimental {
        CPPTRACE_EXPORT void set_dwarf_resolver_line_table_cache_size(nullable<std::size_t> max_entries);
        CPPTRACE_EXPORT void set_dwarf_resolver_disable_aranges(bool disable);
    }

    // dbghelp
    #ifdef _WIN32
     CPPTRACE_EXPORT void load_symbols_for_file(const std::string& filename);
    #endif
CPPTRACE_END_NAMESPACE

#endif
