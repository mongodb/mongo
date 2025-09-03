#include <ctrace/ctrace.h>
#include <cpptrace/cpptrace.hpp>
#include <algorithm>

#include "symbols/symbols.hpp"
#include "unwind/unwind.hpp"
#include "demangle/demangle.hpp"
#include "platform/exception_type.hpp"
#include "utils/common.hpp"
#include "utils/utils.hpp"
#include "binary/object.hpp"
#include "binary/safe_dl.hpp"
#include "utils/string_view.hpp"

#define ESC     "\033["
#define RESET   ESC "0m"
#define RED     ESC "31m"
#define GREEN   ESC "32m"
#define YELLOW  ESC "33m"
#define BLUE    ESC "34m"
#define MAGENTA ESC "35m"
#define CYAN    ESC "36m"

#if defined(__GNUC__) && ((__GNUC__ > 2) || (__GNUC__ == 2 && __GNUC_MINOR__ >= 6))
# define CTRACE_GNU_FORMAT(...) __attribute__((format(__VA_ARGS__)))
#elif defined(__clang__)
// Probably requires llvm >3.5? Not exactly sure.
# define CTRACE_GNU_FORMAT(...) __attribute__((format(__VA_ARGS__)))
#else
# define CTRACE_GNU_FORMAT(...)
#endif

#if defined(__clang__)
# define CTRACE_FORMAT_PROLOGUE         \
    _Pragma("clang diagnostic push")    \
    _Pragma("clang diagnostic ignored \"-Wformat-security\"")
# define CTRACE_FORMAT_EPILOGUE         \
    _Pragma("clang diagnostic pop")
#elif defined(__GNUC_MINOR__)
# define CTRACE_FORMAT_PROLOGUE         \
    _Pragma("GCC diagnostic push")      \
    _Pragma("GCC diagnostic ignored \"-Wformat-security\"")
# define CTRACE_FORMAT_EPILOGUE         \
    _Pragma("GCC diagnostic pop")
#else
# define CTRACE_FORMAT_PROLOGUE
# define CTRACE_FORMAT_EPILOGUE
#endif

namespace ctrace {
    static constexpr std::uint32_t invalid_pos = ~0U;

CTRACE_FORMAT_PROLOGUE
    template <typename...Args>
    CTRACE_GNU_FORMAT(printf, 2, 0)
    static void ffprintf(std::FILE* f, const char fmt[], Args&&...args) {
        (void)std::fprintf(f, fmt, args...);
        (void)fflush(f);
    }
CTRACE_FORMAT_EPILOGUE

    static bool is_empty(std::uint32_t pos) noexcept {
        return pos == invalid_pos;
    }

    static bool is_empty(const char* str) noexcept {
        return !str || std::char_traits<char>::length(str) == 0;
    }

    static ctrace_owning_string generate_owning_string(cpptrace::detail::string_view raw_string) noexcept {
        // Returns length to the null terminator.
        char* new_string = new char[raw_string.size() + 1];
        std::char_traits<char>::copy(new_string, raw_string.data(), raw_string.size());
        new_string[raw_string.size()] = '\0';
        return { new_string };
    }

    static void free_owning_string(const char* owned_string) noexcept {
        if(!owned_string) return; // Not necessary but eh
        delete[] owned_string;
    }

    static void free_owning_string(ctrace_owning_string& owned_string) noexcept {
        free_owning_string(owned_string.data);
    }

    static ctrace_object_frame convert_object_frame(const cpptrace::object_frame& frame) {
        const char* new_path = generate_owning_string(frame.object_path).data;
        return { frame.raw_address, frame.object_address, new_path };
    }

    static ctrace_object_trace c_convert(const std::vector<cpptrace::object_frame>& trace) {
        std::size_t count = trace.size();
        auto* frames = new ctrace_object_frame[count];
        std::transform(trace.begin(), trace.end(), frames, convert_object_frame);
        return { frames, count };
    }

    static ctrace_stacktrace_frame convert_stacktrace_frame(const cpptrace::stacktrace_frame& frame) {
        ctrace_stacktrace_frame new_frame;
        new_frame.raw_address    = frame.raw_address;
        new_frame.object_address = frame.object_address;
        new_frame.line      = frame.line.value_or(invalid_pos);
        new_frame.column    = frame.column.value_or(invalid_pos);
        new_frame.filename  = generate_owning_string(frame.filename).data;
        new_frame.symbol    = generate_owning_string(cpptrace::detail::demangle(frame.symbol, true)).data;
        new_frame.is_inline = ctrace_bool(frame.is_inline);
        return new_frame;
    }

    static cpptrace::stacktrace_frame convert_stacktrace_frame(const ctrace_stacktrace_frame& frame) {
        using nullable_type = cpptrace::nullable<std::uint32_t>;
        static constexpr auto null_v = nullable_type::null().raw_value;
        cpptrace::stacktrace_frame new_frame;
        new_frame.raw_address    = frame.raw_address;
        new_frame.object_address = frame.object_address;
        new_frame.line      = nullable_type{is_empty(frame.line)   ? null_v : frame.line};
        new_frame.column    = nullable_type{is_empty(frame.column) ? null_v : frame.column};
        new_frame.filename  = frame.filename;
        new_frame.symbol    = frame.symbol;
        new_frame.is_inline = bool(frame.is_inline);
        return new_frame;
    }

    static ctrace_stacktrace c_convert(const std::vector<cpptrace::stacktrace_frame>& trace) {
        std::size_t count = trace.size();
        auto* frames = new ctrace_stacktrace_frame[count];
        std::transform(
            trace.begin(),
            trace.end(), frames,
            static_cast<ctrace_stacktrace_frame(*)(const cpptrace::stacktrace_frame&)>(convert_stacktrace_frame)
        );
        return { frames, count };
    }

    static cpptrace::stacktrace cpp_convert(const ctrace_stacktrace* ptrace) {
        if(!ptrace || !ptrace->frames) {
            return { };
        }
        std::vector<cpptrace::stacktrace_frame> new_frames;
        new_frames.reserve(ptrace->count);
        for(std::size_t i = 0; i < ptrace->count; ++i) {
            new_frames.push_back(convert_stacktrace_frame(ptrace->frames[i]));
        }
        return cpptrace::stacktrace{std::move(new_frames)};
    }
}

extern "C" {
    // ctrace::string
    ctrace_owning_string ctrace_generate_owning_string(const char* raw_string) {
        return ctrace::generate_owning_string(raw_string);
    }

    void ctrace_free_owning_string(ctrace_owning_string* string) {
        if(!string) {
            return;
        }
        ctrace::free_owning_string(*string);
        string->data = nullptr;
    }

    // ctrace::generation:
    CTRACE_FORCE_NO_INLINE
    ctrace_raw_trace ctrace_generate_raw_trace(size_t skip, size_t max_depth) {
        try {
            std::vector<cpptrace::frame_ptr> trace = cpptrace::detail::capture_frames(skip + 1, max_depth);
            std::size_t count = trace.size();
            auto* frames = new ctrace_frame_ptr[count];
            std::copy(trace.data(), trace.data() + count, frames);
            return { frames, count };
        } catch(...) {
            // Don't check rethrow condition, it's risky.
            return { nullptr, 0 };
        }
    }

    CTRACE_FORCE_NO_INLINE
    ctrace_object_trace ctrace_generate_object_trace(size_t skip, size_t max_depth) {
        try {
            std::vector<cpptrace::object_frame> trace = cpptrace::detail::get_frames_object_info(
                cpptrace::detail::capture_frames(skip + 1, max_depth)
            );
            return ctrace::c_convert(trace);
        } catch(...) { // NOSONAR
            // Don't check rethrow condition, it's risky.
            return { nullptr, 0 };
        }
    }

    CTRACE_FORCE_NO_INLINE
    ctrace_stacktrace ctrace_generate_trace(size_t skip, size_t max_depth) {
        try {
            std::vector<cpptrace::frame_ptr> frames = cpptrace::detail::capture_frames(skip + 1, max_depth);
            std::vector<cpptrace::stacktrace_frame> trace = cpptrace::detail::resolve_frames(frames);
            return ctrace::c_convert(trace);
        } catch(...) { // NOSONAR
            // Don't check rethrow condition, it's risky.
            return { nullptr, 0 };
        }
    }


    // ctrace::freeing:
    void ctrace_free_raw_trace(ctrace_raw_trace* trace) {
        if(!trace) {
            return;
        }
        ctrace_frame_ptr* frames = trace->frames;
        delete[] frames;
        trace->frames = nullptr;
        trace->count = 0;
    }

    void ctrace_free_object_trace(ctrace_object_trace* trace) {
        if(!trace || !trace->frames) {
            return;
        }
        ctrace_object_frame* frames = trace->frames;
        for(std::size_t i = 0; i < trace->count; ++i) {
            const char* path = frames[i].obj_path;
            ctrace::free_owning_string(path);
        }

        delete[] frames;
        trace->frames = nullptr;
        trace->count = 0;
    }

    void ctrace_free_stacktrace(ctrace_stacktrace* trace) {
        if(!trace || !trace->frames) {
            return;
        }
        ctrace_stacktrace_frame* frames = trace->frames;
        for(std::size_t i = 0; i < trace->count; ++i) {
            ctrace::free_owning_string(frames[i].filename);
            ctrace::free_owning_string(frames[i].symbol);
        }

        delete[] frames;
        trace->frames = nullptr;
        trace->count = 0;
    }

    // ctrace::resolve:
    ctrace_stacktrace ctrace_resolve_raw_trace(const ctrace_raw_trace* trace) {
        if(!trace || !trace->frames) {
            return { nullptr, 0 };
        }
        try {
            std::vector<cpptrace::frame_ptr> frames(trace->count, 0);
            std::copy(trace->frames, trace->frames + trace->count, frames.begin());
            std::vector<cpptrace::stacktrace_frame> resolved = cpptrace::detail::resolve_frames(frames);
            return ctrace::c_convert(resolved);
        } catch(...) { // NOSONAR
            // Don't check rethrow condition, it's risky.
            return { nullptr, 0 };
        }
    }

    ctrace_object_trace ctrace_resolve_raw_trace_to_object_trace(const ctrace_raw_trace* trace) {
        if(!trace || !trace->frames) {
            return { nullptr, 0 };
        }
        try {
            std::vector<cpptrace::frame_ptr> frames(trace->count, 0);
            std::copy(trace->frames, trace->frames + trace->count, frames.begin());
            std::vector<cpptrace::object_frame> obj = cpptrace::detail::get_frames_object_info(frames);
            return ctrace::c_convert(obj);
        } catch(...) { // NOSONAR
            // Don't check rethrow condition, it's risky.
            return { nullptr, 0 };
        }
    }

    ctrace_stacktrace ctrace_resolve_object_trace(const ctrace_object_trace* trace) {
        if(!trace || !trace->frames) {
            return { nullptr, 0 };
        }
        try {
            std::vector<cpptrace::frame_ptr> frames(trace->count, 0);
            std::transform(
                trace->frames,
                trace->frames + trace->count,
                frames.begin(),
                [] (const ctrace_object_frame& frame) -> cpptrace::frame_ptr {
                    return frame.raw_address;
                }
            );
            std::vector<cpptrace::stacktrace_frame> resolved = cpptrace::detail::resolve_frames(frames);
            return ctrace::c_convert(resolved);
        } catch(...) { // NOSONAR
            // Don't check rethrow condition, it's risky.
            return { nullptr, 0 };
        }
    }

    // ctrace::safe:
    size_t ctrace_safe_generate_raw_trace(ctrace_frame_ptr* buffer, size_t size, size_t skip, size_t max_depth) {
        return cpptrace::safe_generate_raw_trace(buffer, size, skip, max_depth);
    }

    void ctrace_get_safe_object_frame(ctrace_frame_ptr address, ctrace_safe_object_frame* out) {
        // TODO: change this?
        static_assert(sizeof(cpptrace::safe_object_frame) == sizeof(ctrace_safe_object_frame), "");
        cpptrace::get_safe_object_frame(address, reinterpret_cast<cpptrace::safe_object_frame*>(out));
    }

    ctrace_bool ctrace_can_signal_safe_unwind() {
        return cpptrace::can_signal_safe_unwind();
    }

    ctrace_bool ctrace_can_get_safe_object_frame(void) {
        return cpptrace::can_get_safe_object_frame();
    }

    // ctrace::io:
    ctrace_owning_string ctrace_stacktrace_to_string(const ctrace_stacktrace* trace, ctrace_bool use_color) {
        if(!trace || !trace->frames) {
            return ctrace::generate_owning_string("<empty trace>");
        }
        auto cpp_trace = ctrace::cpp_convert(trace);
        std::string trace_string = cpp_trace.to_string(bool(use_color));
        return ctrace::generate_owning_string(trace_string);
    }

    void ctrace_print_stacktrace(const ctrace_stacktrace* trace, FILE* to, ctrace_bool use_color) {
        if(
            use_color && (
                (to == stdout && cpptrace::isatty(cpptrace::stdout_fileno)) ||
                (to == stderr && cpptrace::isatty(cpptrace::stderr_fileno))
            )
        ) {
            cpptrace::detail::enable_virtual_terminal_processing_if_needed();
        }
        ctrace::ffprintf(to, "Stack trace (most recent call first):\n");
        if(trace->count == 0 || !trace->frames) {
            ctrace::ffprintf(to, "<empty trace>\n");
            return;
        }
        const auto reset   = use_color ? ESC "0m" : "";
        const auto green   = use_color ? ESC "32m" : "";
        const auto yellow  = use_color ? ESC "33m" : "";
        const auto blue    = use_color ? ESC "34m" : "";
        const auto frame_number_width = cpptrace::detail::n_digits(cpptrace::detail::to<unsigned>(trace->count - 1));
        ctrace_stacktrace_frame* frames = trace->frames;
        for(std::size_t i = 0; i < trace->count; ++i) {
            static constexpr auto ptr_len = 2 * sizeof(cpptrace::frame_ptr);
            ctrace::ffprintf(to, "#%-*llu ", int(frame_number_width), i);
            if(frames[i].is_inline) {
                (void)std::fprintf(to, "%*s",
                    int(ptr_len + 2),
                    "(inlined)");
            } else {
                (void)std::fprintf(to, "%s0x%0*llx%s",
                    blue,
                    int(ptr_len),
                    cpptrace::detail::to_ull(frames[i].raw_address),
                    reset);
            }
            if(!ctrace::is_empty(frames[i].symbol)) {
                (void)std::fprintf(to, " in %s%s%s",
                    yellow,
                    frames[i].symbol,
                    reset);
            }
            if(!ctrace::is_empty(frames[i].filename)) {
                (void)std::fprintf(to, " at %s%s%s",
                    green,
                    frames[i].filename,
                    reset);
                if(ctrace::is_empty(frames[i].line)) {
                    ctrace::ffprintf(to, "\n");
                    continue;
                }
                (void)std::fprintf(to, ":%s%llu%s",
                    blue,
                    cpptrace::detail::to_ull(frames[i].line),
                    reset);
                if(ctrace::is_empty(frames[i].column)) {
                    ctrace::ffprintf(to, "\n");
                    continue;
                }
                (void)std::fprintf(to, ":%s%llu%s",
                    blue,
                    cpptrace::detail::to_ull(frames[i].column),
                    reset);
            }
            // always print newline at end :M
            ctrace::ffprintf(to, "\n");
        }
    }

    // utility::demangle:
    ctrace_owning_string ctrace_demangle(const char* mangled) {
        if(!mangled) {
            return ctrace::generate_owning_string("");
        }
        std::string demangled = cpptrace::demangle(mangled);
        return ctrace::generate_owning_string(demangled);
    }

    // utility::io
    int ctrace_stdin_fileno(void) {
        return cpptrace::stdin_fileno;
    }

    int ctrace_stderr_fileno(void) {
        return cpptrace::stderr_fileno;
    }

    int ctrace_stdout_fileno(void) {
        return cpptrace::stdout_fileno;
    }

    ctrace_bool ctrace_isatty(int fd) {
        return cpptrace::isatty(fd);
    }

    // utility::cache:
    void ctrace_set_cache_mode(ctrace_cache_mode mode) {
        static constexpr auto cache_max = cpptrace::cache_mode::prioritize_speed;
        if(mode > unsigned(cache_max)) {
            return;
        }
        auto cache_mode = static_cast<cpptrace::cache_mode>(mode);
        cpptrace::experimental::set_cache_mode(cache_mode);
    }

    void ctrace_enable_inlined_call_resolution(ctrace_bool enable) {
        cpptrace::enable_inlined_call_resolution(enable);
    }

    ctrace_object_frame ctrace_get_object_info(const ctrace_stacktrace_frame* frame) {
        try {
            cpptrace::object_frame new_frame = cpptrace::detail::get_frame_object_info(frame->raw_address);
            return ctrace::convert_object_frame(new_frame);
        } catch(...) {
            return {0, 0, nullptr};
        }
    }
}
