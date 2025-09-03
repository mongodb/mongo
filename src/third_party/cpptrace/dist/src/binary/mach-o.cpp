#include "binary/mach-o.hpp"

#include "utils/common.hpp"
#include "utils/utils.hpp"
#include "utils/io/file.hpp"
#include "utils/io/memory_file_view.hpp"

#if IS_APPLE

// A number of mach-o functions are deprecated as of macos 13
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include <iostream>
#include <iomanip>

#include <mach-o/loader.h>
#include <mach-o/swap.h>
#include <mach-o/fat.h>
#include <crt_externs.h>
#include <mach-o/nlist.h>
#include <mach-o/stab.h>
#include <mach-o/arch.h>

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
    bool is_mach_o(std::uint32_t magic) {
        switch(magic) {
            case FAT_MAGIC:
            case FAT_CIGAM:
            case MH_MAGIC:
            case MH_CIGAM:
            case MH_MAGIC_64:
            case MH_CIGAM_64:
                return true;
            default:
                return false;
        }
    }

    bool file_is_mach_o(cstring_view object_path) noexcept {
        auto file = raii_wrap(std::fopen(object_path.c_str(), "rb"), file_deleter);
        if(file == nullptr) {
            return false;
        }
        auto magic = load_bytes<std::uint32_t>(file, 0);
        if(magic) {
            return is_mach_o(magic.unwrap_value());
        } else {
            return false;
        }
    }

    bool is_fat_magic(std::uint32_t magic) {
        return magic == FAT_MAGIC || magic == FAT_CIGAM;
    }

    // Based on https://github.com/AlexDenisov/segment_dumper/blob/master/main.c
    // and https://lowlevelbits.org/parsing-mach-o-files/
    bool is_magic_64(std::uint32_t magic) {
        return magic == MH_MAGIC_64 || magic == MH_CIGAM_64;
    }

    bool should_swap_bytes(std::uint32_t magic) {
        return magic == MH_CIGAM || magic == MH_CIGAM_64 || magic == FAT_CIGAM;
    }

    void swap_mach_header(mach_header_64& header) {
        swap_mach_header_64(&header, NX_UnknownByteOrder);
    }

    void swap_mach_header(mach_header& header) {
        swap_mach_header(&header, NX_UnknownByteOrder);
    }

    void swap_segment_command(segment_command_64& segment) {
        swap_segment_command_64(&segment, NX_UnknownByteOrder);
    }

    void swap_segment_command(segment_command& segment) {
        swap_segment_command(&segment, NX_UnknownByteOrder);
    }

    void swap_nlist(struct nlist& entry) {
        swap_nlist(&entry, 1, NX_UnknownByteOrder);
    }

    void swap_nlist(struct nlist_64& entry) {
        swap_nlist_64(&entry, 1, NX_UnknownByteOrder);
    }

    #ifdef __LP64__
     #define LP(x) x##_64
    #else
     #define LP(x) x
    #endif

    Result<const char*, internal_error> mach_o::symtab_info_data::get_string(std::size_t index) const {
        if(stringtab && index < symtab.strsize) {
            return stringtab.unwrap().data() + index;
        } else {
            return internal_error("can't retrieve symbol from symtab");
        }
    }

    Result<monostate, internal_error> mach_o::load() {
        if(magic == FAT_MAGIC || magic == FAT_CIGAM) {
            return load_fat_mach();
        } else {
            fat_index = 0;
            if(is_magic_64(magic)) {
                return load_mach<64>();
            } else {
                return load_mach<32>();
            }
        }
    }

    Result<mach_o, internal_error> mach_o::open(std::unique_ptr<base_file> file) {
        auto magic = file->read<std::uint32_t>(0);
        if(!magic) {
            return magic.unwrap_error();
        }
        if(!is_mach_o(magic.unwrap_value())) {
            return internal_error("File is not mach-o {}", file->path());
        }
        mach_o obj(std::move(file), magic.unwrap_value());
        auto result = obj.load();
        if(result.is_error()) {
            return result.unwrap_error();
        } else {
            return obj;
        }
    }

    Result<mach_o, internal_error> mach_o::open(cstring_view object_path) {
        auto file_res = file::open(object_path);
        if(!file_res) {
            return internal_error("Unable to read object file {}", object_path);
        }
        auto& file = file_res.unwrap_value();
        return open(make_unique(std::move(file)));
    }

    Result<mach_o, internal_error> mach_o::open(cbspan object) {
        return open(make_unique<memory_file_view>(object));
    }

    Result<std::uintptr_t, internal_error> mach_o::get_text_vmaddr() {
        for(const auto& command : load_commands) {
            if(command.cmd == LC_SEGMENT_64 || command.cmd == LC_SEGMENT) {
                auto segment = command.cmd == LC_SEGMENT_64
                                    ? load_segment_command<64>(command.file_offset)
                                    : load_segment_command<32>(command.file_offset);
                if(segment.is_error()) {
                    return std::move(segment).unwrap_error();
                }
                if(std::strcmp(segment.unwrap_value().segname, "__TEXT") == 0) {
                    return segment.unwrap_value().vmaddr;
                }
            }
        }
        // somehow no __TEXT section was found...
        return internal_error("Couldn't find __TEXT section while parsing Mach-O object");
    }

    std::size_t mach_o::get_fat_index() const {
        VERIFY(fat_index != std::numeric_limits<std::size_t>::max());
        return fat_index;
    }

    void mach_o::print_segments() const {
        int i = 0;
        for(const auto& command : load_commands) {
            if(command.cmd == LC_SEGMENT_64 || command.cmd == LC_SEGMENT) {
                auto segment_load = command.cmd == LC_SEGMENT_64
                                    ? load_segment_command<64>(command.file_offset)
                                    : load_segment_command<32>(command.file_offset);
                fprintf(stderr, "Load command %d\n", i);
                if(segment_load.is_error()) {
                    fprintf(stderr, "         error\n");
                    segment_load.drop_error();
                    continue;
                }
                auto& segment = segment_load.unwrap_value();
                fprintf(stderr, "         cmd %u\n", segment.cmd);
                fprintf(stderr, "     cmdsize %u\n", segment.cmdsize);
                fprintf(stderr, "     segname %s\n", segment.segname);
                fprintf(stderr, "      vmaddr 0x%llx\n", segment.vmaddr);
                fprintf(stderr, "      vmsize 0x%llx\n", segment.vmsize);
                fprintf(stderr, "         off 0x%llx\n", segment.fileoff);
                fprintf(stderr, "    filesize %llu\n", segment.filesize);
                fprintf(stderr, "      nsects %u\n", segment.nsects);
            }
            i++;
        }
    }

    Result<std::vector<mach_o::pc_range>, internal_error> mach_o::get_pc_ranges() {
        std::vector<pc_range> ranges;
        for(const auto& command : load_commands) {
            if(command.cmd == LC_SEGMENT_64 || command.cmd == LC_SEGMENT) {
                auto segment_res = command.cmd == LC_SEGMENT_64
                                    ? load_segment_command<64>(command.file_offset)
                                    : load_segment_command<32>(command.file_offset);
                if(segment_res.is_error()) {
                    return std::move(segment_res).unwrap_error();
                }
                auto& segment = segment_res.unwrap_value();
                if(std::strcmp(segment.segname, "__TEXT") == 0) {
                    ranges.push_back({segment.vmaddr, segment.vmaddr + segment.vmsize});
                }
            }
        }
        return ranges;
    }

    Result<std::reference_wrapper<optional<mach_o::symtab_info_data>>, internal_error> mach_o::get_symtab_info() {
        if(!symtab_info.has_value() && !tried_to_load_symtab) {
            // don't try to load the symtab again if for some reason loading here fails
            tried_to_load_symtab = true;
            for(const auto& command : load_commands) {
                if(command.cmd == LC_SYMTAB) {
                    symtab_info_data info;
                    auto symtab = load_symbol_table_command(command.file_offset);
                    if(!symtab) {
                        return std::move(symtab).unwrap_error();
                    }
                    info.symtab = symtab.unwrap_value();
                    auto string = load_string_table(info.symtab.stroff, info.symtab.strsize);
                    if(!string) {
                        return std::move(string).unwrap_error();
                    }
                    info.stringtab = std::move(string).unwrap_value();
                    symtab_info = std::move(info);
                    break;
                }
            }
        }
        return std::reference_wrapper<optional<symtab_info_data>>{symtab_info};
    }

    void mach_o::print_symbol_table_entry(
        const nlist_64& entry,
        const char* stringtab,
        std::size_t stringsize,
        std::size_t j
    ) const {
        const char* type = "";
        if(entry.n_type & N_STAB) {
            switch(entry.n_type) {
                case N_SO: type = "N_SO"; break;
                case N_OSO: type = "N_OSO"; break;
                case N_BNSYM: type = "N_BNSYM"; break;
                case N_ENSYM: type = "N_ENSYM"; break;
                case N_FUN: type = "N_FUN"; break;
            }
        } else if((entry.n_type & N_TYPE) == N_SECT) {
            type = "N_SECT";
        }
        fprintf(
            stderr,
            "%5llu %8llx %2llx %7s %2llu %4llx %16llx %s\n",
            to_ull(j),
            to_ull(entry.n_un.n_strx),
            to_ull(entry.n_type),
            type,
            to_ull(entry.n_sect),
            to_ull(entry.n_desc),
            to_ull(entry.n_value),
            stringtab == nullptr
                ? "Stringtab error"
                : entry.n_un.n_strx < stringsize
                    ? stringtab + entry.n_un.n_strx
                    : "String index out of bounds"
        );
    }

    void mach_o::print_symbol_table() {
        int i = 0;
        for(const auto& command : load_commands) {
            if(command.cmd == LC_SYMTAB) {
                auto symtab_load = load_symbol_table_command(command.file_offset);
                fprintf(stderr, "Load command %d\n", i);
                if(symtab_load.is_error()) {
                    fprintf(stderr, "         error\n");
                    symtab_load.drop_error();
                    continue;
                }
                auto& symtab = symtab_load.unwrap_value();
                fprintf(stderr, "         cmd %llu\n", to_ull(symtab.cmd));
                fprintf(stderr, "     cmdsize %llu\n", to_ull(symtab.cmdsize));
                fprintf(stderr, "      symoff 0x%llu\n", to_ull(symtab.symoff));
                fprintf(stderr, "       nsyms %llu\n", to_ull(symtab.nsyms));
                fprintf(stderr, "      stroff 0x%llu\n", to_ull(symtab.stroff));
                fprintf(stderr, "     strsize %llu\n", to_ull(symtab.strsize));
                auto stringtab = load_string_table(symtab.stroff, symtab.strsize);
                if(!stringtab) {
                    stringtab.drop_error();
                }
                for(std::size_t j = 0; j < symtab.nsyms; j++) {
                    auto entry = bits == 32
                                    ? load_symtab_entry<32>(symtab.symoff, j)
                                    : load_symtab_entry<64>(symtab.symoff, j);
                    if(!entry) {
                        fprintf(stderr, "error loading symtab entry\n");
                        entry.drop_error();
                        continue;
                    }
                    print_symbol_table_entry(
                        entry.unwrap_value(),
                        stringtab ? stringtab.unwrap_value().data() : nullptr,
                        symtab.strsize,
                        j
                    );
                }
            }
            i++;
        }
    }

    // produce information similar to dsymutil -dump-debug-map
    Result<mach_o::debug_map, internal_error> mach_o::get_debug_map() {
        // we have a bunch of symbols in our binary we need to pair up with symbols from various .o files
        // first collect symbols and the objects they come from
        debug_map debug_map;
        auto symtab_info_res = get_symtab_info();
        if(!symtab_info_res) {
            return std::move(symtab_info_res).unwrap_error();
        }
        if(!symtab_info_res.unwrap_value().get()) {
            return internal_error("No symtab info");
        }
        const auto& symtab_info = symtab_info_res.unwrap_value().get().unwrap();
        const auto& symtab = symtab_info.symtab;
        // TODO: Take timestamp into account?
        std::string current_module;
        optional<debug_map_entry> current_function;
        for(std::size_t j = 0; j < symtab.nsyms; j++) {
            auto load_entry = bits == 32
                            ? load_symtab_entry<32>(symtab.symoff, j)
                            : load_symtab_entry<64>(symtab.symoff, j);
            if(!load_entry) {
                return std::move(load_entry).unwrap_error();
            }
            auto& entry = load_entry.unwrap_value();
            // entry.n_type & N_STAB indicates symbolic debug info
            if(!(entry.n_type & N_STAB)) {
                continue;
            }
            switch(entry.n_type) {
                case N_SO:
                    // pass - these encode path and filename for the module, if applicable
                    break;
                case N_OSO:
                    {
                        // sets the module
                        auto str = symtab_info.get_string(entry.n_un.n_strx);
                        if(!str) {
                            return std::move(str).unwrap_error();
                        }
                        current_module = str.unwrap_value();
                    }
                    break;
                case N_BNSYM: break; // pass
                case N_ENSYM: break; // pass
                case N_FUN:
                    {
                        auto str = symtab_info.get_string(entry.n_un.n_strx);
                        if(!str) {
                            return std::move(str).unwrap_error();
                        }
                        if(str.unwrap_value()[0] == 0) {
                            // end of function scope
                            if(!current_function) { /**/ }
                            current_function.unwrap().size = entry.n_value;
                            debug_map[current_module].push_back(std::move(current_function).unwrap());
                        } else {
                            current_function = debug_map_entry{};
                            current_function.unwrap().source_address = entry.n_value;
                            current_function.unwrap().name = str.unwrap_value();
                        }
                    }
                    break;
            }
        }
        return debug_map;
    }

    Result<const std::vector<mach_o::symbol_entry>&, internal_error> mach_o::symbol_table() {
        if(symbols) {
            return symbols.unwrap();
        }
        if(tried_to_load_symbols) {
            return internal_error("previous symbol table load failed");
        }
        tried_to_load_symbols = true;
        std::vector<symbol_entry> symbol_table;
        // we have a bunch of symbols in our binary we need to pair up with symbols from various .o files
        // first collect symbols and the objects they come from
        auto symtab_info_res = get_symtab_info();
        if(!symtab_info_res) {
            return std::move(symtab_info_res).unwrap_error();
        }
        if(!symtab_info_res.unwrap_value().get()) {
            return internal_error("No symtab info");
        }
        const auto& symtab_info = symtab_info_res.unwrap_value().get().unwrap();
        const auto& symtab = symtab_info.symtab;
        // TODO: Take timestamp into account?
        for(std::size_t j = 0; j < symtab.nsyms; j++) {
            auto load_entry = bits == 32
                            ? load_symtab_entry<32>(symtab.symoff, j)
                            : load_symtab_entry<64>(symtab.symoff, j);
            if(!load_entry) {
                return std::move(load_entry).unwrap_error();
            }
            auto& entry = load_entry.unwrap_value();
            if(entry.n_type & N_STAB) {
                continue;
            }
            if((entry.n_type & N_TYPE) == N_SECT) {
                auto str = symtab_info.get_string(entry.n_un.n_strx);
                if(!str) {
                    return std::move(str).unwrap_error();
                }
                symbol_table.push_back({
                    entry.n_value,
                    str.unwrap_value()
                });
            }
        }
        std::sort(
            symbol_table.begin(),
            symbol_table.end(),
            [] (const symbol_entry& a, const symbol_entry& b) { return a.address < b.address; }
        );
        symbols = std::move(symbol_table);
        return symbols.unwrap();
    }

    optional<std::string> mach_o::lookup_symbol(frame_ptr pc) {
        auto symtab_ = symbol_table();
        if(!symtab_) {
            return nullopt;
        }
        const auto& symtab = symtab_.unwrap_value();;
        auto it = first_less_than_or_equal(
            symtab.begin(),
            symtab.end(),
            pc,
            [] (frame_ptr pc, const symbol_entry& entry) {
                return pc < entry.address;
            }
        );
        if(it == symtab.end()) {
            return nullopt;
        }
        ASSERT(pc >= it->address);
        // TODO: We subtracted one from the address so name + diff won't show up in the objdump, decide if desirable
        // to have an easier offset to lookup
        return microfmt::format("{} + {}", it->name, pc - it->address);
    }

    // produce information similar to dsymutil -dump-debug-map
    void mach_o::print_debug_map(const debug_map& debug_map) {
        for(const auto& entry : debug_map) {
            std::cout<<entry.first<<": "<< '\n';
            for(const auto& symbol : entry.second) {
                std::cerr
                    << "    "
                    << symbol.name
                    << " "
                    << std::hex
                    << symbol.source_address
                    << " "
                    << symbol.size
                    << std::dec
                    << '\n';
            }
        }
    }

    template<std::size_t Bits>
    Result<monostate, internal_error> mach_o::load_mach() {
        static_assert(Bits == 32 || Bits == 64, "Unexpected Bits argument");
        bits = Bits;
        using Mach_Header = typename std::conditional<Bits == 32, mach_header, mach_header_64>::type;
        std::size_t header_size = sizeof(Mach_Header);
        auto load_header = file->read<Mach_Header>(load_base);
        if(!load_header) {
            return load_header.unwrap_error();
        }
        Mach_Header& header = load_header.unwrap_value();
        magic = header.magic;
        if(should_swap()) {
            swap_mach_header(header);
        }
        cputype = header.cputype;
        cpusubtype = header.cpusubtype;
        filetype = header.filetype;
        n_load_commands = header.ncmds;
        sizeof_load_commands = header.sizeofcmds;
        flags = header.flags;
        // handle load commands
        std::uint32_t ncmds = header.ncmds;
        std::uint32_t load_commands_offset = load_base + header_size;
        // iterate load commands
        std::uint32_t actual_offset = load_commands_offset;
        for(std::uint32_t i = 0; i < ncmds; i++) {
            auto load_cmd = file->read<load_command>(actual_offset);
            if(!load_cmd) {
                return load_cmd.unwrap_error();
            }
            load_command& cmd = load_cmd.unwrap_value();
            if(should_swap()) {
                swap_load_command(&cmd, NX_UnknownByteOrder);
            }
            load_commands.push_back({ actual_offset, cmd.cmd, cmd.cmdsize });
            actual_offset += cmd.cmdsize;
        }
        return monostate{};
    }

    Result<monostate, internal_error> mach_o::load_fat_mach() {
        std::size_t header_size = sizeof(fat_header);
        std::size_t arch_size = sizeof(fat_arch);
        auto load_header = file->read<fat_header>(0);
        if(!load_header) {
            return load_header.unwrap_error();
        }
        fat_header& header = load_header.unwrap_value();
        if(should_swap()) {
            swap_fat_header(&header, NX_UnknownByteOrder);
        }
        // thread_local static struct LP(mach_header)* mhp = _NSGetMachExecuteHeader();
        // off_t arch_offset = (off_t)header_size;
        // for(std::size_t i = 0; i < header.nfat_arch; i++) {
        //     fat_arch arch = load_bytes<fat_arch>(file, arch_offset);
        //     if(should_swap()) {
        //         swap_fat_arch(&arch, 1, NX_UnknownByteOrder);
        //     }
        //     off_t mach_header_offset = (off_t)arch.offset;
        //     arch_offset += arch_size;
        //     std::uint32_t magic = load_bytes<std::uint32_t>(file, mach_header_offset);
        //     std::cerr<<"xxx: "<<arch.cputype<<" : "<<mhp->cputype<<std::endl;
        //     std::cerr<<"     "<<arch.cpusubtype<<" : "<<static_cast<cpu_subtype_t>(mhp->cpusubtype & ~CPU_SUBTYPE_MASK)<<std::endl;
        //     if(
        //         arch.cputype == mhp->cputype &&
        //         static_cast<cpu_subtype_t>(mhp->cpusubtype & ~CPU_SUBTYPE_MASK) == arch.cpusubtype
        //     ) {
        //         load_base = mach_header_offset;
        //         fat_index = i;
        //         if(is_magic_64(magic)) {
        //             load_mach<64>(true);
        //         } else {
        //             load_mach<32>(true);
        //         }
        //         return;
        //     }
        // }
        std::vector<fat_arch> fat_arches;
        fat_arches.reserve(header.nfat_arch);
        off_t arch_offset = (off_t)header_size;
        for(std::size_t i = 0; i < header.nfat_arch; i++) {
            auto load_arch = file->read<fat_arch>(arch_offset);
            if(!load_arch) {
                return load_arch.unwrap_error();
            }
            fat_arch& arch = load_arch.unwrap_value();
            if(should_swap()) {
                swap_fat_arch(&arch, 1, NX_UnknownByteOrder);
            }
            fat_arches.push_back(arch);
            arch_offset += arch_size;
        }
        thread_local static struct LP(mach_header)* mhp = _NSGetMachExecuteHeader();
        fat_arch* best = NXFindBestFatArch(
            mhp->cputype,
            mhp->cpusubtype,
            fat_arches.data(),
            header.nfat_arch
        );
        if(best) {
            off_t mach_header_offset = (off_t)best->offset;
            auto magic = file->read<std::uint32_t>(mach_header_offset);
            if(!magic) {
                return magic.unwrap_error();
            }
            load_base = mach_header_offset;
            fat_index = best - fat_arches.data();
            if(is_magic_64(magic.unwrap_value())) {
                load_mach<64>();
            } else {
                load_mach<32>();
            }
            return monostate{};
        }
        // If this is reached... something went wrong. The cpu we're on wasn't found.
        return internal_error("Couldn't find appropriate architecture in fat Mach-O");
    }

    template<std::size_t Bits>
    Result<segment_command_64, internal_error> mach_o::load_segment_command(std::uint32_t offset) const {
        using Segment_Command = typename std::conditional<Bits == 32, segment_command, segment_command_64>::type;
        auto load_segment = file->read<Segment_Command>(offset);
        if(!load_segment) {
            return load_segment.unwrap_error();
        }
        Segment_Command& segment = load_segment.unwrap_value();
        ASSERT(segment.cmd == LC_SEGMENT_64 || segment.cmd == LC_SEGMENT);
        if(should_swap()) {
            swap_segment_command(segment);
        }
        // fields match just u64 instead of u32
        segment_command_64 common;
        common.cmd = segment.cmd;
        common.cmdsize = segment.cmdsize;
        static_assert(sizeof common.segname == 16 && sizeof segment.segname == 16, "xx");
        memcpy(common.segname, segment.segname, 16);
        common.vmaddr = segment.vmaddr;
        common.vmsize = segment.vmsize;
        common.fileoff = segment.fileoff;
        common.filesize = segment.filesize;
        common.maxprot = segment.maxprot;
        common.initprot = segment.initprot;
        common.nsects = segment.nsects;
        common.flags = segment.flags;
        return common;
    }

    Result<symtab_command, internal_error> mach_o::load_symbol_table_command(std::uint32_t offset) const {
        auto load_symtab = file->read<symtab_command>(offset);
        if(!load_symtab) {
            return load_symtab.unwrap_error();
        }
        symtab_command& symtab = load_symtab.unwrap_value();
        ASSERT(symtab.cmd == LC_SYMTAB);
        if(should_swap()) {
            swap_symtab_command(&symtab, NX_UnknownByteOrder);
        }
        return symtab;
    }

    template<std::size_t Bits>
    Result<nlist_64, internal_error> mach_o::load_symtab_entry(std::uint32_t symbol_base, std::size_t index) const {
        using Nlist = typename std::conditional<Bits == 32, struct nlist, struct nlist_64>::type;
        uint32_t offset = load_base + symbol_base + index * sizeof(Nlist);
        auto load_entry = file->read<Nlist>(offset);
        if(!load_entry) {
            return load_entry.unwrap_error();
        }
        Nlist& entry = load_entry.unwrap_value();
        if(should_swap()) {
            swap_nlist(entry);
        }
        // fields match just u64 instead of u32
        nlist_64 common;
        common.n_un.n_strx = entry.n_un.n_strx;
        common.n_type = entry.n_type;
        common.n_sect = entry.n_sect;
        common.n_desc = entry.n_desc;
        common.n_value = entry.n_value;
        return common;
    }

    Result<std::vector<char>, internal_error> mach_o::load_string_table(std::uint32_t offset, std::uint32_t byte_count) const {
        std::vector<char> buffer(byte_count + 1);
        auto read_res = file->read_bytes(span<char>{buffer.data(), byte_count}, load_base + offset);
        if(!read_res) {
            return read_res.unwrap_error();
        }
        buffer[byte_count] = 0; // just out of an abundance of caution
        return buffer;
    }

    bool mach_o::should_swap() const {
        return should_swap_bytes(magic);
    }

    Result<bool, internal_error> macho_is_fat(cstring_view object_path) {
        auto file = raii_wrap(std::fopen(object_path.c_str(), "rb"), file_deleter);
        if(file == nullptr) {
            return internal_error("Unable to read object file {}", object_path);
        }
        auto magic = load_bytes<std::uint32_t>(file, 0);
        if(!magic) {
            return magic.unwrap_error();
        } else {
            return is_fat_magic(magic.unwrap_value());
        }
    }

    Result<maybe_owned<mach_o>, internal_error> open_mach_o_cached(const std::string& object_path) {
        if(object_path.empty()) {
            return internal_error{"empty object_path"};
        }
        if(get_cache_mode() == cache_mode::prioritize_memory) {
            return mach_o::open(object_path)
                .transform([](mach_o&& obj) {
                    return maybe_owned<mach_o>{detail::make_unique<mach_o>(std::move(obj))};
                });
        } else {
            static std::mutex m;
            std::unique_lock<std::mutex> lock{m};
            // TODO: Re-evaluate storing the error
            static std::unordered_map<std::string, Result<mach_o, internal_error>> cache;
            auto it = cache.find(object_path);
            if(it == cache.end()) {
                auto res = cache.insert({ object_path, mach_o::open(object_path) });
                VERIFY(res.second);
                it = res.first;
            }
            return it->second.transform([](mach_o& obj) { return maybe_owned<mach_o>(&obj); });
        }
    }
}
CPPTRACE_END_NAMESPACE

#pragma GCC diagnostic pop

#endif
