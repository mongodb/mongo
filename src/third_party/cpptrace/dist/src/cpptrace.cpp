#include <cpptrace/cpptrace.hpp>
#include <cpptrace/formatting.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <fstream>

#include "cpptrace/basic.hpp"
#include "jit/jit_objects.hpp"
#include "symbols/symbols.hpp"
#include "unwind/unwind.hpp"
#include "demangle/demangle.hpp"
#include "utils/common.hpp"
#include "utils/microfmt.hpp"
#include "utils/utils.hpp"
#include "binary/object.hpp"
#include "binary/safe_dl.hpp"
#include "snippets/snippet.hpp"
#include "options.hpp"

CPPTRACE_BEGIN_NAMESPACE
    CPPTRACE_FORCE_NO_INLINE
    raw_trace raw_trace::current(std::size_t skip) {
        try { // try/catch can never be hit but it's needed to prevent TCO
            return generate_raw_trace(skip + 1);
        } catch(...) {
            detail::log_and_maybe_propagate_exception(std::current_exception());
            return raw_trace{};
        }
    }

    CPPTRACE_FORCE_NO_INLINE
    raw_trace raw_trace::current(std::size_t skip, std::size_t max_depth) {
        try { // try/catch can never be hit but it's needed to prevent TCO
            return generate_raw_trace(skip + 1, max_depth);
        } catch(...) {
            detail::log_and_maybe_propagate_exception(std::current_exception());
            return raw_trace{};
        }
    }

    object_trace raw_trace::resolve_object_trace() const {
        try {
            return object_trace{detail::get_frames_object_info(frames)};
        } catch(...) { // NOSONAR
            detail::log_and_maybe_propagate_exception(std::current_exception());
            return object_trace{};
        }
    }

    stacktrace raw_trace::resolve() const {
        try {
            std::vector<stacktrace_frame> trace = detail::resolve_frames(frames);
            for(auto& frame : trace) {
                frame.symbol = detail::demangle(frame.symbol, true);
            }
            return {std::move(trace)};
        } catch(...) { // NOSONAR
            detail::log_and_maybe_propagate_exception(std::current_exception());
            return stacktrace{};
        }
    }

    void raw_trace::clear() {
        frames.clear();
    }

    bool raw_trace::empty() const noexcept {
        return frames.empty();
    }

    CPPTRACE_FORCE_NO_INLINE
    object_trace object_trace::current(std::size_t skip) {
        try { // try/catch can never be hit but it's needed to prevent TCO
            return generate_object_trace(skip + 1);
        } catch(...) {
            detail::log_and_maybe_propagate_exception(std::current_exception());
            return object_trace{};
        }
    }

    CPPTRACE_FORCE_NO_INLINE
    object_trace object_trace::current(std::size_t skip, std::size_t max_depth) {
        try { // try/catch can never be hit but it's needed to prevent TCO
            return generate_object_trace(skip + 1, max_depth);
        } catch(...) {
            detail::log_and_maybe_propagate_exception(std::current_exception());
            return object_trace{};
        }
    }

    stacktrace object_trace::resolve() const {
        try {
            std::vector<stacktrace_frame> trace = detail::resolve_frames(frames);
            for(auto& frame : trace) {
                frame.symbol = detail::demangle(frame.symbol, true);
            }
            return {std::move(trace)};
        } catch(...) { // NOSONAR
            detail::log_and_maybe_propagate_exception(std::current_exception());
            return stacktrace();
        }
    }

    void object_trace::clear() {
        frames.clear();
    }

    bool object_trace::empty() const noexcept {
        return frames.empty();
    }

    object_frame stacktrace_frame::get_object_info() const {
        return detail::get_frame_object_info(raw_address);
    }

    std::string stacktrace_frame::to_string() const {
        return to_string(false);
    }

    std::string stacktrace_frame::to_string(bool color) const {
        return get_default_formatter().format(*this, color);
    }

    std::ostream& operator<<(std::ostream& stream, const stacktrace_frame& frame) {
        return stream << frame.to_string();
    }

    CPPTRACE_FORCE_NO_INLINE
    stacktrace stacktrace::current(std::size_t skip) {
        try { // try/catch can never be hit but it's needed to prevent TCO
            return generate_trace(skip + 1);
        } catch(...) {
            detail::log_and_maybe_propagate_exception(std::current_exception());
            return stacktrace{};
        }
    }

    CPPTRACE_FORCE_NO_INLINE
    stacktrace stacktrace::current(std::size_t skip, std::size_t max_depth) {
        try { // try/catch can never be hit but it's needed to prevent TCO
            return generate_trace(skip + 1, max_depth);
        } catch(...) {
            detail::log_and_maybe_propagate_exception(std::current_exception());
            return stacktrace{};
        }
    }

    void stacktrace::print() const {
        get_default_formatter().print(*this);
    }

    void stacktrace::print(std::ostream& stream) const {
        get_default_formatter().print(stream, *this);
    }

    void stacktrace::print(std::ostream& stream, bool color) const {
        get_default_formatter().print(stream, *this, color);
    }

    namespace detail {
        const formatter& get_default_snippet_formatter() {
            static formatter snippet_formatter = formatter{}.snippets(true);
            return snippet_formatter;
        }
    }

    void stacktrace::print_with_snippets() const {
        detail::get_default_snippet_formatter().print(*this);
    }

    void stacktrace::print_with_snippets(std::ostream& stream) const {
        detail::get_default_snippet_formatter().print(stream, *this);
    }

    void stacktrace::print_with_snippets(std::ostream& stream, bool color) const {
        detail::get_default_snippet_formatter().print(stream, *this, color);
    }

    void stacktrace::clear() {
        frames.clear();
    }

    bool stacktrace::empty() const noexcept {
        return frames.empty();
    }

    std::string stacktrace::to_string(bool color) const {
        return get_default_formatter().format(*this, color);
    }

    std::ostream& operator<<(std::ostream& stream, const stacktrace& trace) {
        get_default_formatter().print(stream, trace);
        return stream;
    }

    CPPTRACE_FORCE_NO_INLINE
    raw_trace generate_raw_trace(std::size_t skip) {
        try {
            return raw_trace{detail::capture_frames(skip + 1, SIZE_MAX)};
        } catch(...) { // NOSONAR
            detail::log_and_maybe_propagate_exception(std::current_exception());
            return raw_trace{};
        }
    }

    CPPTRACE_FORCE_NO_INLINE
    raw_trace generate_raw_trace(std::size_t skip, std::size_t max_depth) {
        try {
            return raw_trace{detail::capture_frames(skip + 1, max_depth)};
        } catch(...) { // NOSONAR
            detail::log_and_maybe_propagate_exception(std::current_exception());
            return raw_trace{};
        }
    }

    CPPTRACE_FORCE_NO_INLINE
    std::size_t safe_generate_raw_trace(frame_ptr* buffer, std::size_t size, std::size_t skip) {
        try { // try/catch can never be hit but it's needed to prevent TCO
            return detail::safe_capture_frames(buffer, size, skip + 1, SIZE_MAX);
        } catch(...) {
            detail::log_and_maybe_propagate_exception(std::current_exception());
            return 0;
        }
    }

    CPPTRACE_FORCE_NO_INLINE
    std::size_t safe_generate_raw_trace(
         frame_ptr* buffer,
         std::size_t size,
         std::size_t skip,
         std::size_t max_depth
    ) {
        try { // try/catch can never be hit but it's needed to prevent TCO
            return detail::safe_capture_frames(buffer, size, skip + 1, max_depth);
        } catch(...) {
            detail::log_and_maybe_propagate_exception(std::current_exception());
            return 0;
        }
    }

    CPPTRACE_FORCE_NO_INLINE
    object_trace generate_object_trace(std::size_t skip) {
        try {
            return object_trace{detail::get_frames_object_info(detail::capture_frames(skip + 1, SIZE_MAX))};
        } catch(...) { // NOSONAR
            detail::log_and_maybe_propagate_exception(std::current_exception());
            return object_trace{};
        }
    }

    CPPTRACE_FORCE_NO_INLINE
    object_trace generate_object_trace(std::size_t skip, std::size_t max_depth) {
        try {
            return object_trace{detail::get_frames_object_info(detail::capture_frames(skip + 1, max_depth))};
        } catch(...) { // NOSONAR
            detail::log_and_maybe_propagate_exception(std::current_exception());
            return object_trace{};
        }
    }

    CPPTRACE_FORCE_NO_INLINE
    stacktrace generate_trace(std::size_t skip) {
        try { // try/catch can never be hit but it's needed to prevent TCO
            return generate_trace(skip + 1, SIZE_MAX);
        } catch(...) {
            detail::log_and_maybe_propagate_exception(std::current_exception());
            return stacktrace{};
        }
    }

    CPPTRACE_FORCE_NO_INLINE
    stacktrace generate_trace(std::size_t skip, std::size_t max_depth) {
        try {
            std::vector<frame_ptr> frames = detail::capture_frames(skip + 1, max_depth);
            std::vector<stacktrace_frame> trace = detail::resolve_frames(frames);
            for(auto& frame : trace) {
                frame.symbol = detail::demangle(frame.symbol, true);
            }
            return {std::move(trace)};
        } catch(...) { // NOSONAR
            detail::log_and_maybe_propagate_exception(std::current_exception());
            return stacktrace();
        }
    }

    object_frame safe_object_frame::resolve() const {
        return detail::resolve_safe_object_frame(*this);
    }

    void get_safe_object_frame(frame_ptr address, safe_object_frame* out) {
        detail::get_safe_object_frame(address, out);
    }

    bool can_signal_safe_unwind() {
        return detail::has_safe_unwind();
    }

    bool can_get_safe_object_frame() {
        return detail::has_get_safe_object_frame();
    }

    void register_jit_object(const char* ptr, std::size_t size) {
        detail::register_jit_object(ptr, size);
    }

    void unregister_jit_object(const char* ptr) {
        detail::unregister_jit_object(ptr);
    }

    void clear_all_jit_objects() {
        detail::clear_all_jit_objects();
    }
CPPTRACE_END_NAMESPACE
