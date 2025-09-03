#ifndef MACHO_HPP
#define MACHO_HPP

#include "utils/common.hpp"
#include "utils/utils.hpp"
#include "utils/span.hpp"
#include "utils/io/base_file.hpp"

#if IS_APPLE

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <mach-o/arch.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
    bool file_is_mach_o(cstring_view object_path) noexcept;

    struct load_command_entry {
        std::uint32_t file_offset;
        std::uint32_t cmd;
        std::uint32_t cmdsize;
    };

    class mach_o {
    public:
        struct debug_map_entry {
            uint64_t source_address;
            uint64_t size;
            std::string name;
        };

        struct symbol_entry {
            uint64_t address;
            std::string name;
        };

        // map from object file to a vector of symbols to resolve
        using debug_map = std::unordered_map<std::string, std::vector<debug_map_entry>>;

    private:
        std::unique_ptr<base_file> file;
        std::uint32_t magic;
        cpu_type_t cputype;
        cpu_subtype_t cpusubtype;
        std::uint32_t filetype;
        std::uint32_t n_load_commands;
        std::uint32_t sizeof_load_commands;
        std::uint32_t flags;
        std::size_t bits = 0; // 32 or 64 once load_mach is called

        std::size_t load_base = 0;
        std::size_t fat_index = std::numeric_limits<std::size_t>::max();

        std::vector<load_command_entry> load_commands;

        struct symtab_info_data {
            symtab_command symtab;
            optional<std::vector<char>> stringtab;
            Result<const char*, internal_error> get_string(std::size_t index) const;
        };

        bool tried_to_load_symtab = false;
        optional<symtab_info_data> symtab_info;

        bool tried_to_load_symbols = false;
        optional<std::vector<symbol_entry>> symbols;

        mach_o(std::unique_ptr<base_file> file, std::uint32_t magic) : file(std::move(file)), magic(magic) {}

        Result<monostate, internal_error> load();

        static NODISCARD Result<mach_o, internal_error> open(std::unique_ptr<base_file> file);

    public:
        static NODISCARD Result<mach_o, internal_error> open(cstring_view object_path);
        static NODISCARD Result<mach_o, internal_error> open(cbspan object);

        mach_o(mach_o&&) = default;
        ~mach_o() = default;

        Result<std::uintptr_t, internal_error> get_text_vmaddr();

        std::size_t get_fat_index() const;

        void print_segments() const;

        struct pc_range {
            frame_ptr low;
            frame_ptr high; // not inclusive
        };
        // for in-memory JIT mach-o's
        Result<std::vector<pc_range>, internal_error> get_pc_ranges();

        Result<std::reference_wrapper<optional<symtab_info_data>>, internal_error> get_symtab_info();

        void print_symbol_table_entry(
            const nlist_64& entry,
            const char* stringtab,
            std::size_t stringsize,
            std::size_t j
        ) const;

        void print_symbol_table();

        // produce information similar to dsymutil -dump-debug-map
        Result<debug_map, internal_error> get_debug_map();

        Result<const std::vector<symbol_entry>&, internal_error> symbol_table();

        optional<std::string> lookup_symbol(frame_ptr pc);

        // produce information similar to dsymutil -dump-debug-map
        static void print_debug_map(const debug_map& debug_map);

    private:
        template<std::size_t Bits>
        Result<monostate, internal_error> load_mach();

        Result<monostate, internal_error> load_fat_mach();

        template<std::size_t Bits>
        Result<segment_command_64, internal_error> load_segment_command(std::uint32_t offset) const;

        Result<symtab_command, internal_error> load_symbol_table_command(std::uint32_t offset) const;

        template<std::size_t Bits>
        Result<nlist_64, internal_error> load_symtab_entry(std::uint32_t symbol_base, std::size_t index) const;

        Result<std::vector<char>, internal_error> load_string_table(std::uint32_t offset, std::uint32_t byte_count) const;

        bool should_swap() const;
    };

    Result<bool, internal_error> macho_is_fat(cstring_view object_path);

    NODISCARD Result<maybe_owned<mach_o>, internal_error> open_mach_o_cached(const std::string& object_path);
}
CPPTRACE_END_NAMESPACE

#endif

#endif
