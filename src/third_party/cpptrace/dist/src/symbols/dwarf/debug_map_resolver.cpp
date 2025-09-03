#ifdef CPPTRACE_GET_SYMBOLS_WITH_LIBDWARF

#include "symbols/dwarf/resolver.hpp"

#include <cpptrace/basic.hpp>
#include "symbols/symbols.hpp"
#include "utils/common.hpp"
#include "utils/error.hpp"
#include "binary/object.hpp"
#include "binary/mach-o.hpp"
#include "utils/utils.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
namespace libdwarf {
    #if IS_APPLE
    struct target_object {
        std::string object_path;
        bool path_ok = true;
        optional<std::unordered_map<std::string, uint64_t>> symbols;
        std::unique_ptr<symbol_resolver> resolver;

        target_object(std::string object_path) : object_path(std::move(object_path)) {}

        std::unique_ptr<symbol_resolver>& get_resolver() {
            if(!resolver) {
                // this seems silly but it's an attempt to not repeatedly try to initialize new dwarf_resolvers if
                // exceptions are thrown, e.g. if the path doesn't exist
                resolver = detail::make_unique<null_resolver>();
                resolver = make_dwarf_resolver(object_path);
            }
            return resolver;
        }

        std::unordered_map<std::string, uint64_t>& get_symbols() {
            if(!symbols) {
                // this is an attempt to not repeatedly try to reprocess mach-o files if exceptions are thrown, e.g. if
                // the path doesn't exist
                std::unordered_map<std::string, uint64_t> symbols;
                this->symbols = symbols;
                auto mach_o_object = open_mach_o_cached(object_path);
                if(!mach_o_object) {
                    return this->symbols.unwrap();
                }
                const auto& symbol_table = mach_o_object.unwrap_value()->symbol_table();
                if(!symbol_table) {
                    return this->symbols.unwrap();
                }
                for(const auto& symbol : symbol_table.unwrap_value()) {
                    symbols[symbol.name] = symbol.address;
                }
                this->symbols = std::move(symbols);
            }
            return symbols.unwrap();
        }

        CPPTRACE_FORCE_NO_INLINE_FOR_PROFILING
        frame_with_inlines resolve_frame(
            const object_frame& frame_info,
            const std::string& symbol_name,
            std::size_t offset
        ) {
            const auto& symbol_table = get_symbols();
            auto it = symbol_table.find(symbol_name);
            if(it != symbol_table.end()) {
                auto frame = frame_info;
                // substitute a translated address object for the target file in
                frame.object_address = it->second + offset;
                auto res = get_resolver()->resolve_frame(frame);
                // replace the translated address with the object address in the binary
                res.frame.object_address = frame_info.object_address;
                return res;
            } else {
                return {
                    {
                        frame_info.raw_address,
                        frame_info.object_address,
                        nullable<std::uint32_t>::null(),
                        nullable<std::uint32_t>::null(),
                        frame_info.object_path,
                        symbol_name,
                        false
                    },
                    {}
                };
            }
        }
    };

    struct debug_map_symbol_info {
        uint64_t source_address;
        uint64_t size;
        std::string name;
        nullable<uint64_t> target_address; // T(-1) is used as a sentinel
        std::size_t object_index; // index into target_objects
    };

    class debug_map_resolver : public symbol_resolver {
        std::vector<target_object> target_objects;
        std::vector<debug_map_symbol_info> symbols;
    public:
        debug_map_resolver(const std::string& source_object_path) {
            // load mach-o
            // TODO: Cache somehow?
            auto mach_o_object = open_mach_o_cached(source_object_path);
            if(!mach_o_object) {
                return;
            }
            mach_o& source_mach = *mach_o_object.unwrap_value();
            auto source_debug_map = source_mach.get_debug_map();
            if(!source_debug_map) {
                return;
            }
            // get symbol entries from debug map, as well as the various object files used to make this binary
            for(auto& entry : source_debug_map.unwrap_value()) {
                // object it came from
                target_objects.push_back({entry.first});
                // push the symbols
                auto& map_entry_symbols = entry.second;
                symbols.reserve(symbols.size() + map_entry_symbols.size());
                for(auto& symbol : map_entry_symbols) {
                    symbols.push_back({
                        symbol.source_address,
                        symbol.size,
                        std::move(symbol.name),
                        nullable<uint64_t>::null(),
                        target_objects.size() - 1
                    });
                }
            }
            // sort for binary lookup later
            // TODO: Redundant?
            std::sort(
                symbols.begin(),
                symbols.end(),
                [] (
                    const debug_map_symbol_info& a,
                    const debug_map_symbol_info& b
                ) {
                    return a.source_address < b.source_address;
                }
            );
        }
        CPPTRACE_FORCE_NO_INLINE_FOR_PROFILING
        frame_with_inlines resolve_frame(const object_frame& frame_info) override {
            // resolve object frame:
            //   find the symbol in this executable corresponding to the object address
            //   resolve the symbol in the object it came from, based on the symbol name
            auto closest_symbol_it = first_less_than_or_equal(
                symbols.begin(),
                symbols.end(),
                frame_info.object_address,
                [] (
                    uint64_t pc,
                    const debug_map_symbol_info& symbol
                ) {
                    return pc < symbol.source_address;
                }
            );
            if(closest_symbol_it != symbols.end()) {
                if(frame_info.object_address <= closest_symbol_it->source_address + closest_symbol_it->size) {
                    return target_objects[closest_symbol_it->object_index].resolve_frame(
                        {
                            frame_info.raw_address,
                            // the resolver doesn't care about the object address here, only the offset from the start
                            // of the symbol and it'll lookup the symbol's base-address
                            frame_info.object_address,
                            frame_info.object_path
                        },
                        closest_symbol_it->name,
                        frame_info.object_address - closest_symbol_it->source_address
                    );
                }
            }
            // There was either no closest symbol or the closest symbol didn't end up containing the address we're
            // looking for, so just return a blank frame
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

    std::unique_ptr<symbol_resolver> make_debug_map_resolver(const std::string& object_path) {
        return detail::make_unique<debug_map_resolver>(object_path);
    }
    #endif
}
}
CPPTRACE_END_NAMESPACE

#endif
