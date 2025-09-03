#include <cpptrace/basic.hpp>
#include <cpptrace/utils.hpp>

#include "cpptrace/forward.hpp"
#include "symbols/symbols.hpp"

#include <vector>
#include <unordered_map>

#include "utils/error.hpp"
#include "binary/object.hpp"

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
    template<typename CollatedVec, typename Entry>
    std::unordered_map<std::string, CollatedVec> collate_frames(
        const std::vector<object_frame>& frames,
        std::vector<Entry>& trace
    ) {
        std::unordered_map<std::string, CollatedVec> entries;
        for(std::size_t i = 0; i < frames.size(); i++) {
            const auto& entry = frames[i];
            // The path may be empty. This can happens if libdl fails to find the shared object for a frame, e.g. I've
            // observed this on macos when looking up the shared object containing `start`.
            // It can also happen for JIT frames. As such, we don't exclude them from the output.
            entries[entry.object_path].emplace_back(
                entry,
                trace[i]
            );
        }
        return entries;
    }

    std::unordered_map<std::string, collated_vec> collate_frames(
        const std::vector<object_frame>& frames,
        std::vector<stacktrace_frame>& trace
    ) {
        return collate_frames<collated_vec>(frames, trace);
    }
    std::unordered_map<std::string, collated_vec_with_inlines> collate_frames(
        const std::vector<object_frame>& frames,
        std::vector<frame_with_inlines>& trace
    ) {
        return collate_frames<collated_vec_with_inlines>(frames, trace);
    }

    /*
     *
     *
     * All the code here is awful and I'm not proud of it.
     *
     *
     *
     */

    // Resolver must not support walking inlines
    void fill_blanks(
        std::vector<stacktrace_frame>& vec,
        std::vector<stacktrace_frame> (*resolver)(const std::vector<frame_ptr>&)
    ) {
        std::vector<frame_ptr> addresses;
        for(const auto& frame : vec) {
            if(frame.symbol.empty() || frame.filename.empty()) {
                addresses.push_back(frame.raw_address);
            }
        }
        std::vector<stacktrace_frame> new_frames = resolver(addresses);
        std::size_t i = 0;
        for(auto& frame : vec) {
            if(frame.symbol.empty() || frame.filename.empty()) {
                // three cases to handle, either partially overwrite or fully overwrite
                if(frame.symbol.empty() && frame.filename.empty()) {
                    frame = new_frames[i];
                } else if(frame.symbol.empty() && !frame.filename.empty()) {
                    frame.symbol = new_frames[i].symbol;
                } else {
                    ASSERT(!frame.symbol.empty() && frame.filename.empty());
                    frame.filename = new_frames[i].filename;
                    frame.line = new_frames[i].line;
                    frame.column = new_frames[i].column;
                }
                i++;
            }
        }
    }

    // TODO: Symbol resolution code should probably handle when object addresses are 0

    std::vector<stacktrace_frame> resolve_frames(const std::vector<object_frame>& frames) {
        #if defined(CPPTRACE_GET_SYMBOLS_WITH_LIBDWARF) && defined(CPPTRACE_GET_SYMBOLS_WITH_DBGHELP)
         std::vector<stacktrace_frame> trace = libdwarf::resolve_frames(frames);
         fill_blanks(trace, dbghelp::resolve_frames);
         return trace;
        #else
         #if defined(CPPTRACE_GET_SYMBOLS_WITH_LIBDL) \
             || defined(CPPTRACE_GET_SYMBOLS_WITH_DBGHELP) \
             || defined(CPPTRACE_GET_SYMBOLS_WITH_LIBBACKTRACE)
          // actually need to go backwards to a void*
          std::vector<frame_ptr> raw_frames(frames.size());
          for(std::size_t i = 0; i < frames.size(); i++) {
              raw_frames[i] = frames[i].raw_address;
          }
         #endif
         #ifdef CPPTRACE_GET_SYMBOLS_WITH_LIBDL
          return libdl::resolve_frames(raw_frames);
         #endif
         #ifdef CPPTRACE_GET_SYMBOLS_WITH_LIBDWARF
          return libdwarf::resolve_frames(frames);
         #endif
         #ifdef CPPTRACE_GET_SYMBOLS_WITH_DBGHELP
          return dbghelp::resolve_frames(raw_frames);
         #endif
         #ifdef CPPTRACE_GET_SYMBOLS_WITH_ADDR2LINE
          return addr2line::resolve_frames(frames);
         #endif
         #ifdef CPPTRACE_GET_SYMBOLS_WITH_LIBBACKTRACE
          return libbacktrace::resolve_frames(raw_frames);
         #endif
         #ifdef CPPTRACE_GET_SYMBOLS_WITH_NOTHING
          return nothing::resolve_frames(frames);
         #endif
        #endif
    }

    std::vector<stacktrace_frame> resolve_frames(const std::vector<frame_ptr>& frames) {
        #if defined(CPPTRACE_GET_SYMBOLS_WITH_LIBDWARF) \
            || defined(CPPTRACE_GET_SYMBOLS_WITH_ADDR2LINE)
         auto dlframes = get_frames_object_info(frames);
        #endif
        #if defined(CPPTRACE_GET_SYMBOLS_WITH_LIBDWARF) && defined(CPPTRACE_GET_SYMBOLS_WITH_DBGHELP)
         std::vector<stacktrace_frame> trace = libdwarf::resolve_frames(dlframes);
         fill_blanks(trace, dbghelp::resolve_frames);
         return trace;
        #else
         #ifdef CPPTRACE_GET_SYMBOLS_WITH_LIBDL
          return libdl::resolve_frames(frames);
         #endif
         #ifdef CPPTRACE_GET_SYMBOLS_WITH_LIBDWARF
          return libdwarf::resolve_frames(dlframes);
         #endif
         #ifdef CPPTRACE_GET_SYMBOLS_WITH_DBGHELP
          return dbghelp::resolve_frames(frames);
         #endif
         #ifdef CPPTRACE_GET_SYMBOLS_WITH_ADDR2LINE
          return addr2line::resolve_frames(dlframes);
         #endif
         #ifdef CPPTRACE_GET_SYMBOLS_WITH_LIBBACKTRACE
          return libbacktrace::resolve_frames(frames);
         #endif
         #ifdef CPPTRACE_GET_SYMBOLS_WITH_NOTHING
          return nothing::resolve_frames(frames);
         #endif
        #endif
    }
}
CPPTRACE_END_NAMESPACE
