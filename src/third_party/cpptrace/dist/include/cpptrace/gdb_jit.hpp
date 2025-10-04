#ifndef CPPTRACE_GDB_JIT_HPP
#define CPPTRACE_GDB_JIT_HPP

#include <cpptrace/basic.hpp>

#include <cstdint>

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
    // https://sourceware.org/gdb/current/onlinedocs/gdb.html/JIT-Interface.html
    extern "C" {
        typedef enum
        {
            JIT_NOACTION = 0,
            JIT_REGISTER_FN,
            JIT_UNREGISTER_FN
        } jit_actions_t;

        struct jit_code_entry
        {
            struct jit_code_entry *next_entry;
            struct jit_code_entry *prev_entry;
            const char *symfile_addr;
            uint64_t symfile_size;
        };

        struct jit_descriptor
        {
            uint32_t version;
            /* This type should be jit_actions_t, but we use uint32_t
                to be explicit about the bitwidth.  */
            uint32_t action_flag;
            struct jit_code_entry *relevant_entry;
            struct jit_code_entry *first_entry;
        };

        extern struct jit_descriptor __jit_debug_descriptor;
    }
}

namespace experimental {
    inline void register_jit_objects_from_gdb_jit_interface() {
        clear_all_jit_objects();
        detail::jit_code_entry* entry = detail::__jit_debug_descriptor.first_entry;
        while(entry) {
            register_jit_object(entry->symfile_addr, entry->symfile_size);
            entry = entry->next_entry;
        }
    }
}
CPPTRACE_END_NAMESPACE

#endif
