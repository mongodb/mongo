#include "jit/jit_objects.hpp"

#include "cpptrace/forward.hpp"
#include "utils/error.hpp"
#include "utils/optional.hpp"
#include "utils/span.hpp"
#include "binary/elf.hpp"
#include "binary/mach-o.hpp"

#include <algorithm>
#include <iostream>
#include <fstream>
#include <map>
#include <vector>

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
    #if IS_LINUX || IS_APPLE
    class jit_object_manager {
        struct object_entry {
            const char* object_start;
            std::unique_ptr<jit_object_type> object;
        };
        std::vector<object_entry> objects;

        struct range_entry {
            frame_ptr low;
            frame_ptr high; // not inclusive
            const char* object_start;
            jit_object_type* object;
            bool operator<(const range_entry& other) const {
                return low < other.low;
            }
        };
        // TODO: Maybe use a set...
        std::vector<range_entry> range_list;

    public:
        void add_jit_object(cbspan object) {
            auto object_res = jit_object_type::open(object);
            if(object_res.is_error()) {
                if(!should_absorb_trace_exceptions()) {
                    object_res.drop_error();
                }
                return;
            }
            objects.push_back({object.data(), make_unique<jit_object_type>(std::move(object_res).unwrap_value())});
            auto* object_file = objects.back().object.get();
            auto ranges_res = object_file->get_pc_ranges();
            if(ranges_res.is_error()) {
                if(!should_absorb_trace_exceptions()) {
                    ranges_res.drop_error();
                }
                return;
            }
            auto& ranges = ranges_res.unwrap_value();
            for(auto range : ranges) {
                range_entry entry{range.low, range.high, object.data(), object_file};
                // TODO: Perf
                range_list.insert(std::upper_bound(range_list.begin(), range_list.end(), entry), entry);
            }
        }

        void remove_jit_object(const char* ptr) {
            // TODO: Perf
            objects.erase(
                std::remove_if(
                    objects.begin(),
                    objects.end(),
                    [&](const object_entry& entry) { return entry.object_start == ptr; }
                ),
                objects.end()
            );
            range_list.erase(
                std::remove_if(
                    range_list.begin(),
                    range_list.end(),
                    [&](const range_entry& entry) { return entry.object_start == ptr; }
                ),
                range_list.end()
            );
        }

        optional<jit_object_lookup_result> lookup(frame_ptr pc) const {
            auto it = first_less_than_or_equal(
                range_list.begin(),
                range_list.end(),
                pc,
                [](frame_ptr pc, const range_entry& entry) {
                    return pc < entry.low;
                }
            );
            if(it == range_list.end()) {
                return nullopt;
            }
            ASSERT(pc >= it->low);
            if(pc < it->high) {
                return jit_object_lookup_result{*it->object, it->low};
            } else {
                return nullopt;
            }
        }

        void clear_all_jit_objects() {
            objects.clear();
            range_list.clear();
        }
    };
    #else
    class jit_object_manager {
    public:
        void add_jit_object(cbspan) {}
        void remove_jit_object(const char*) {}
        void clear_all_jit_objects() {}
    };
    #endif

    jit_object_manager& get_jit_object_manager() {
        static jit_object_manager manager;
        return manager;
    }

    void register_jit_object(const char* ptr, std::size_t size) {
        auto& manager = get_jit_object_manager();
        manager.add_jit_object(make_span(ptr, size));
    }

    void unregister_jit_object(const char* ptr) {
        auto& manager = get_jit_object_manager();
        manager.remove_jit_object(ptr);
    }

    void clear_all_jit_objects() {
        auto& manager = get_jit_object_manager();
        manager.clear_all_jit_objects();
    }

    #if IS_LINUX || IS_APPLE
    optional<jit_object_lookup_result> lookup_jit_object(frame_ptr pc) {
        return get_jit_object_manager().lookup(pc);
    }
    #endif
}
CPPTRACE_END_NAMESPACE
