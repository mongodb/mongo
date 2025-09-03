#ifndef JIT_OBJECTS_HPP
#define JIT_OBJECTS_HPP

#include "binary/elf.hpp"
#include "binary/mach-o.hpp"
#include "cpptrace/forward.hpp"
#include "utils/optional.hpp"
#include "platform/platform.hpp"

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
    void register_jit_object(const char*, std::size_t);
    void unregister_jit_object(const char*);
    void clear_all_jit_objects();

    #if IS_LINUX || IS_APPLE
    #if IS_LINUX
     using jit_object_type = elf;
    #elif IS_APPLE
     using jit_object_type = mach_o;
    #endif
    struct jit_object_lookup_result {
        jit_object_type& object;
        frame_ptr base;
    };
    optional<jit_object_lookup_result> lookup_jit_object(frame_ptr pc);
    #endif
}
CPPTRACE_END_NAMESPACE

#endif
