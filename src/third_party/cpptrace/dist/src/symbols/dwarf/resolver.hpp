#ifndef SYMBOL_RESOLVER_HPP
#define SYMBOL_RESOLVER_HPP

#include <cpptrace/basic.hpp>
#include "symbols/symbols.hpp"
#include "platform/platform.hpp"
#include "utils/string_view.hpp"

#include <memory>

#if false
 #define CPPTRACE_FORCE_NO_INLINE_FOR_PROFILING CPPTRACE_FORCE_NO_INLINE
#else
 #define CPPTRACE_FORCE_NO_INLINE_FOR_PROFILING
#endif

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
namespace libdwarf {
    class symbol_resolver {
    public:
        virtual ~symbol_resolver() = default;
        CPPTRACE_FORCE_NO_INLINE_FOR_PROFILING
        virtual frame_with_inlines resolve_frame(const object_frame& frame_info) = 0;
    };

    class null_resolver : public symbol_resolver {
    public:
        explicit null_resolver() = default;
        null_resolver(cstring_view) {}

        CPPTRACE_FORCE_NO_INLINE_FOR_PROFILING
        frame_with_inlines resolve_frame(const object_frame& frame_info) override {
            return {
                {
                    frame_info.raw_address,
                    frame_info.object_address,
                    nullable<std::uint32_t>::null(),
                    nullable<std::uint32_t>::null(),
                    frame_info.object_path,
                    "",
                    false
                },
                {}
            };
        };
    };

    std::unique_ptr<symbol_resolver> make_dwarf_resolver(cstring_view object_path);
    #if IS_APPLE
     std::unique_ptr<symbol_resolver> make_debug_map_resolver(const std::string& object_path);
    #endif
}
}
CPPTRACE_END_NAMESPACE

#endif
