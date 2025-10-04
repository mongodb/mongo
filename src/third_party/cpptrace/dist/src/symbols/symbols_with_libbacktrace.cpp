#ifdef CPPTRACE_GET_SYMBOLS_WITH_LIBBACKTRACE

#include <cpptrace/basic.hpp>
#include "symbols/symbols.hpp"
#include "platform/program_name.hpp"
#include "utils/error.hpp"
#include "utils/common.hpp"
#include "options.hpp"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

#ifdef CPPTRACE_BACKTRACE_PATH
#include CPPTRACE_BACKTRACE_PATH
#else
#include <backtrace.h>
#endif

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
namespace libbacktrace {
    int full_callback(void* data, std::uintptr_t address, const char* file, int line, const char* symbol) {
        stacktrace_frame& frame = *static_cast<stacktrace_frame*>(data);
        frame.raw_address = address;
        frame.line = line;
        frame.filename = file ? file : "";
        frame.symbol = symbol ? symbol : "";
        return 0;
    }

    void syminfo_callback(void* data, std::uintptr_t address, const char* symbol, std::uintptr_t, std::uintptr_t) {
        stacktrace_frame& frame = *static_cast<stacktrace_frame*>(data);
        frame.raw_address = address;
        frame.line = 0;
        frame.filename = "";
        frame.symbol = symbol ? symbol : "";
    }

    void error_callback(void*, const char* msg, int errnum) {
        if(msg == std::string("no debug info in ELF executable")) {
            // https://github.com/jeremy-rifkin/cpptrace/issues/114
            // https://github.com/ianlancetaylor/libbacktrace/blob/ae1e707dbacd4a5cc82fcf2d3816f410e9c5fec4/elf.c#L592
            // not a critical error, just return
            return;
        }
        throw internal_error("Libbacktrace error: {}, code {}", msg, errnum);
    }

    backtrace_state* get_backtrace_state() {
        static std::mutex mutex;
        const std::lock_guard<std::mutex> lock(mutex);
        // backtrace_create_state must be called only one time per program
        static backtrace_state* state = nullptr;
        static bool called = false;
        if(!called) {
            state = backtrace_create_state(program_name(), true, error_callback, nullptr);
            called = true;
        }
        return state;
    }

    // TODO: Handle backtrace_pcinfo calling the callback multiple times on inlined functions
    stacktrace_frame resolve_frame(const frame_ptr addr) {
        try {
            stacktrace_frame frame = null_frame();
            frame.raw_address = addr;
            backtrace_pcinfo(
                get_backtrace_state(),
                addr,
                full_callback,
                error_callback,
                &frame
            );
            if(frame.symbol.empty()) {
                // fallback, try to at least recover the symbol name with backtrace_syminfo
                backtrace_syminfo(
                    get_backtrace_state(),
                    addr,
                    syminfo_callback,
                    error_callback,
                    &frame
                );
            }
            return frame;
        } catch(...) { // NOSONAR
            detail::log_and_maybe_propagate_exception(std::current_exception());
            return null_frame();
        }
    }

    std::vector<stacktrace_frame> resolve_frames(const std::vector<frame_ptr>& frames) {
        std::vector<stacktrace_frame> trace;
        trace.reserve(frames.size());
        for(const auto frame : frames) {
            trace.push_back(resolve_frame(frame));
        }
        return trace;
    }
}
}
CPPTRACE_END_NAMESPACE

#endif
