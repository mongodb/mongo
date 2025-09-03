module;
#include <cpptrace/basic.hpp>
#include <cpptrace/cpptrace.hpp>
#include <cpptrace/exceptions.hpp>
#include <cpptrace/formatting.hpp>
#include <cpptrace/forward.hpp>
#include <cpptrace/from_current.hpp>

export module cpptrace;

CPPTRACE_BEGIN_NAMESPACE
    // cpptrace/basic
    export using cpptrace::raw_trace;
    export using cpptrace::object_frame;
    export using cpptrace::object_trace;
    export using cpptrace::nullable;
    export using cpptrace::stacktrace_frame;
    export using cpptrace::stacktrace;
    export using cpptrace::generate_raw_trace;
    export using cpptrace::generate_object_trace;
    export using cpptrace::generate_trace;
    export using cpptrace::safe_generate_raw_trace;
    export using cpptrace::safe_object_frame;
    export using cpptrace::can_get_safe_object_frame;
    export using cpptrace::can_signal_safe_unwind;
    export using cpptrace::can_get_safe_object_frame;
    export using cpptrace::register_jit_object;
    export using cpptrace::unregister_jit_object;
    export using cpptrace::clear_all_jit_objects;

    // cpptrace/exceptions
    export using cpptrace::exception;
    export using cpptrace::lazy_exception;
    export using cpptrace::exception_with_message;
    export using cpptrace::logic_error;
    export using cpptrace::domain_error;
    export using cpptrace::invalid_argument;
    export using cpptrace::length_error;
    export using cpptrace::out_of_range;
    export using cpptrace::runtime_error;
    export using cpptrace::range_error;
    export using cpptrace::overflow_error;
    export using cpptrace::underflow_error;
    export using cpptrace::nested_exception;
    export using cpptrace::system_error;
    export using cpptrace::rethrow_and_wrap_if_needed;

    // cpptrace/formatting
    export using cpptrace::basename;
    export using cpptrace::prettify_symbol;
    export using cpptrace::formatter;
    export using cpptrace::get_default_formatter;

    // cpptrace/forward
    export using cpptrace::frame_ptr;

    // cpptrace/from_current.hpp
    export using cpptrace::raw_trace_from_current_exception;
    export using cpptrace::from_current_exception;
    export using cpptrace::raw_trace_from_current_exception_rethrow;
    export using cpptrace::from_current_exception_rethrow;
    export using cpptrace::current_exception_was_rethrown;
    export using cpptrace::rethrow;
    export using cpptrace::clear_current_exception_traces;
    export using cpptrace::try_catch;

    namespace detail {
        #ifdef _MSC_VER
         export using cpptrace::detail::argument;
         export using cpptrace::detail::exception_filter;
        #else
         export using cpptrace::detail::unwind_interceptor;
         export using cpptrace::detail::unwind_interceptor_for;
         export using cpptrace::detail::nop;
        #endif
    }

    // cpptrace/io
    export using cpptrace::operator<<; // FIXME: make hidden friend

    // cpptrace/utils
    export using cpptrace::demangle;
    export using cpptrace::prune_symbol;
    export using cpptrace::get_snippet;
    export using cpptrace::isatty;
    export using cpptrace::stdin_fileno;
    export using cpptrace::stderr_fileno;
    export using cpptrace::stdout_fileno;
    export using cpptrace::register_terminate_handler;
    export using cpptrace::absorb_trace_exceptions;
    export using cpptrace::enable_inlined_call_resolution;
    export using cpptrace::cache_mode;
    export using cpptrace::log_level;
    export using cpptrace::set_log_level;
    export using cpptrace::set_log_callback;
    export using cpptrace::use_default_stderr_logger;
    export using cpptrace::to_string;
    export using cpptrace::cache_mode;

    namespace experimental {
        export using cpptrace::experimental::set_cache_mode;
        export using cpptrace::experimental::set_dwarf_resolver_line_table_cache_size;
        export using cpptrace::experimental::set_dwarf_resolver_disable_aranges;
    }

    #ifdef _WIN32
     export using cpptrace::load_symbols_for_file;
    #endif
CPPTRACE_END_NAMESPACE
