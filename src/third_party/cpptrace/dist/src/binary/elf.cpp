#include "binary/elf.hpp"

#include "utils/error.hpp"
#include "utils/io/base_file.hpp"
#include "utils/io/memory_file_view.hpp"
#include "utils/optional.hpp"
#include "utils/io/file.hpp"
#include "utils/string_view.hpp"

#if IS_LINUX

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <type_traits>
#include <unordered_map>

#include <elf.h>

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
    elf::elf(
        std::unique_ptr<base_file> file,
        bool is_little_endian,
        bool is_64
    ) : file(std::move(file)), is_little_endian(is_little_endian), is_64(is_64) {}

    Result<elf, internal_error> elf::open(std::unique_ptr<base_file> file) {
        // Initial checks/metadata
        auto magic = file->read<std::array<char, 4>>(0);
        if(magic.is_error()) {
            return std::move(magic).unwrap_error();
        }
        if(magic.unwrap_value() != (std::array<char, 4>{0x7F, 'E', 'L', 'F'})) {
            return internal_error("File is not ELF {}", file->path());
        }
        auto ei_class = file->read<std::uint8_t>(4);
        if(ei_class.is_error()) {
            return std::move(ei_class).unwrap_error();
        }
        bool is_64 = ei_class.unwrap_value() == 2;
        auto ei_data = file->read<std::uint8_t>(5);
        if(ei_data.is_error()) {
            return std::move(ei_data).unwrap_error();
        }
        bool is_little_endian = ei_data.unwrap_value() == 1;
        auto ei_version = file->read<std::uint8_t>(6);
        if(ei_version.is_error()) {
            return std::move(ei_version).unwrap_error();
        }
        if(ei_version.unwrap_value() != 1) {
            return internal_error("Unexpected ELF version {}", file->path());
        }
        return elf(std::move(file), is_little_endian, is_64);
    }

    Result<elf, internal_error> elf::open(cstring_view object_path) {
        auto file_res = file::open(object_path);
        if(!file_res) {
            return internal_error("Unable to read object file {}", object_path);
        }
        auto& file = file_res.unwrap_value();
        return open(make_unique(std::move(file)));
    }

    Result<elf, internal_error> elf::open(cbspan object) {
        return open(make_unique<memory_file_view>(object));
    }

    Result<std::uintptr_t, internal_error> elf::get_module_image_base() {
        // get image base
        if(is_64) {
            return get_module_image_base_impl<64>();
        } else {
            return get_module_image_base_impl<32>();
        }
    }

    template<std::size_t Bits>
    Result<std::uintptr_t, internal_error> elf::get_module_image_base_impl() {
        static_assert(Bits == 32 || Bits == 64, "Unexpected Bits argument");
        using PHeader = typename std::conditional<Bits == 32, Elf32_Phdr, Elf64_Phdr>::type;
        auto header = get_header_info();
        if(header.is_error()) {
            return std::move(header).unwrap_error();
        }
        const auto& header_info = header.unwrap_value();
        // PT_PHDR will occur at most once
        // Should be somewhat reliable https://stackoverflow.com/q/61568612/15675011
        // It should occur at the beginning but may as well loop just in case
        for(unsigned i = 0; i < header_info.e_phnum; i++) {
            auto loaded_ph = file->read<PHeader>(header_info.e_phoff + header_info.e_phentsize * i);
            if(loaded_ph.is_error()) {
                return std::move(loaded_ph).unwrap_error();
            }
            const PHeader& program_header = loaded_ph.unwrap_value();
            if(byteswap_if_needed(program_header.p_type) == PT_PHDR) {
                return byteswap_if_needed(program_header.p_vaddr) -
                    byteswap_if_needed(program_header.p_offset);
            }
        }
        // Apparently some objects like shared objects can end up missing this header. 0 as a base seems correct.
        return 0;
    }

    optional<std::string> elf::lookup_symbol(frame_ptr pc) {
        if(auto symtab = get_symtab()) {
            if(auto symbol = lookup_symbol(pc, symtab.unwrap_value())) {
                return symbol;
            }
        }
        if(auto dynamic_symtab = get_dynamic_symtab()) {
            if(auto symbol = lookup_symbol(pc, dynamic_symtab.unwrap_value())) {
                return symbol;
            }
        }
        return nullopt;
    }

    optional<std::string> elf::lookup_symbol(frame_ptr pc, const optional<symtab_info>& maybe_symtab) {
        if(!maybe_symtab) {
            return nullopt;
        }
        auto& symtab = maybe_symtab.unwrap();
        if(symtab.strtab_link == SHN_UNDEF) {
            return nullopt;
        }
        auto strtab_ = get_strtab(symtab.strtab_link);
        if(strtab_.is_error()) {
            return nullopt;
        }
        auto& strtab = strtab_.unwrap_value();
        auto it = first_less_than_or_equal(
            symtab.entries.begin(),
            symtab.entries.end(),
            pc,
            [] (frame_ptr pc, const symtab_entry& entry) {
                return pc < entry.st_value;
            }
        );
        if(it == symtab.entries.end()) {
            return nullopt;
        }
        if(pc <= it->st_value + it->st_size) {
            return strtab.data() + it->st_name;
        }
        return nullopt;
    }

    Result<std::vector<elf::pc_range>, internal_error> elf::get_pc_ranges() {
        std::vector<pc_range> vec;
        auto header_info_ = get_header_info();
        if(header_info_.is_error()) {
            return header_info_.unwrap_error();
        }
        auto& header_info = header_info_.unwrap_value();
        auto strtab_ = get_strtab(header_info.e_shstrndx);
        if(strtab_.is_error()) {
            return strtab_.unwrap_error();
        }
        auto& strtab = strtab_.unwrap_value();
        auto sections_res = get_sections();
        if(!sections_res) {
            return sections_res.unwrap_error();
        }
        const auto& sections = sections_res.unwrap_value();
        for(const auto& section : sections) {
            if(string_view(strtab.data() + section.sh_name) == ".text") {
                vec.push_back(
                    pc_range{to<frame_ptr>(section.sh_addr), to<frame_ptr>(section.sh_addr + section.sh_size)}
                );
            }
        }
        return vec;
    }

    Result<optional<std::vector<elf::symbol_entry>>, internal_error> elf::get_symtab_entries() {
        return resolve_symtab_entries(get_symtab());
    }
    Result<optional<std::vector<elf::symbol_entry>>, internal_error> elf::get_dynamic_symtab_entries() {
        return resolve_symtab_entries(get_dynamic_symtab());
    }

    Result<optional<std::vector<elf::symbol_entry>>, internal_error> elf::resolve_symtab_entries(
        const Result<const optional<elf::symtab_info> &, internal_error>& symtab
    ) {
        if(!symtab) {
            return symtab.unwrap_error();
        }
        if(!symtab.unwrap_value()) {
            return nullopt;
        }
        const auto& info = symtab.unwrap_value().unwrap();
        optional<const std::vector<char>&> strtab;
        if(info.strtab_link != SHN_UNDEF) {
            auto strtab_ = get_strtab(info.strtab_link);
            if(strtab_.is_error()) {
                return strtab_.unwrap_error();
            }
            strtab = strtab_.unwrap_value();
        }
        std::vector<symbol_entry> res;
        for(const auto& entry : info.entries) {
            res.push_back({
                strtab.has_value() ? strtab.unwrap().data() + entry.st_name : "<strtab error>",
                entry.st_shndx,
                entry.st_value,
                entry.st_size
            });
        }
        return res;
    }

    template<typename T, typename std::enable_if<std::is_integral<T>::value, int>::type>
    T elf::byteswap_if_needed(T value) {
        if(detail::is_little_endian() == is_little_endian) {
            return value;
        } else {
            return byteswap(value);
        }
    }

    Result<const elf::header_info&, internal_error> elf::get_header_info() {
        if(header) {
            return header.unwrap();
        }
        if(tried_to_load_header) {
            return internal_error("previous header load failed {}", file->path());
        }
        tried_to_load_header = true;
        if(is_64) {
            return get_header_info_impl<64>();
        } else {
            return get_header_info_impl<32>();
        }
    }

    template<std::size_t Bits>
    Result<const elf::header_info&, internal_error> elf::get_header_info_impl() {
        static_assert(Bits == 32 || Bits == 64, "Unexpected Bits argument");
        using Header = typename std::conditional<Bits == 32, Elf32_Ehdr, Elf64_Ehdr>::type;
        auto loaded_header = file->read<Header>(0);
        if(loaded_header.is_error()) {
            return std::move(loaded_header).unwrap_error();
        }
        const Header& file_header = loaded_header.unwrap_value();
        if(file_header.e_ehsize != sizeof(Header)) {
            return internal_error("ELF file header size mismatch {}", file->path());
        }
        header_info info;
        info.e_phoff = byteswap_if_needed(file_header.e_phoff);
        info.e_phnum = byteswap_if_needed(file_header.e_phnum);
        info.e_phentsize = byteswap_if_needed(file_header.e_phentsize);
        info.e_shoff = byteswap_if_needed(file_header.e_shoff);
        info.e_shnum = byteswap_if_needed(file_header.e_shnum);
        info.e_shentsize = byteswap_if_needed(file_header.e_shentsize);
        info.e_shstrndx = byteswap_if_needed(file_header.e_shstrndx);
        header = info;
        return header.unwrap();
    }

    Result<const std::vector<elf::section_info>&, internal_error> elf::get_sections() {
        if(did_load_sections) {
            return sections;
        }
        if(tried_to_load_sections) {
            return internal_error("previous sections load failed {}", file->path());
        }
        tried_to_load_sections = true;
        if(is_64) {
            return get_sections_impl<64>();
        } else {
            return get_sections_impl<32>();
        }
    }

    template<std::size_t Bits>
    Result<const std::vector<elf::section_info>&, internal_error> elf::get_sections_impl() {
        static_assert(Bits == 32 || Bits == 64, "Unexpected Bits argument");
        using SHeader = typename std::conditional<Bits == 32, Elf32_Shdr, Elf64_Shdr>::type;
        auto header = get_header_info();
        if(header.is_error()) {
            return std::move(header).unwrap_error();
        }
        const auto& header_info = header.unwrap_value();
        for(unsigned i = 0; i < header_info.e_shnum; i++) {
            auto loaded_sh = file->read<SHeader>(header_info.e_shoff + header_info.e_shentsize * i);
            if(loaded_sh.is_error()) {
                return std::move(loaded_sh).unwrap_error();
            }
            const SHeader& section_header = loaded_sh.unwrap_value();
            section_info info;
            info.sh_name = byteswap_if_needed(section_header.sh_name);
            info.sh_type = byteswap_if_needed(section_header.sh_type);
            info.sh_addr = byteswap_if_needed(section_header.sh_addr);
            info.sh_offset = byteswap_if_needed(section_header.sh_offset);
            info.sh_size = byteswap_if_needed(section_header.sh_size);
            info.sh_entsize = byteswap_if_needed(section_header.sh_entsize);
            info.sh_link = byteswap_if_needed(section_header.sh_link);
            sections.push_back(info);
        }
        did_load_sections = true;
        return sections;
    }

    Result<const std::vector<char>&, internal_error> elf::get_strtab(std::size_t index) {
        auto res = strtab_entries.insert({index, {}});
        auto it = res.first;
        auto did_insert = res.second;
        auto& entry = it->second;
        if(!did_insert) {
            if(entry.did_load_strtab) {
                return entry.data;
            }
            if(entry.tried_to_load_strtab) {
                return internal_error("previous strtab load failed {}", file->path());
            }
        }
        entry.tried_to_load_strtab = true;
        auto sections_ = get_sections();
        if(sections_.is_error()) {
            return std::move(sections_).unwrap_error();
        }
        const auto& sections = sections_.unwrap_value();
        if(index >= sections.size()) {
            return internal_error("requested strtab section index out of range");
        }
        const auto& section = sections[index];
        if(section.sh_type != SHT_STRTAB) {
            return internal_error("requested strtab section not a strtab (requested {} of {})", index, file->path());
        }
        entry.data.resize(section.sh_size + 1);
        auto read_res = file->read_bytes(
            span<char>{entry.data.data(), to<std::size_t>(section.sh_size)},
            section.sh_offset
        );
        if(!read_res) {
            return read_res.unwrap_error();
        }
        entry.data[section.sh_size] = 0; // just out of an abundance of caution
        entry.did_load_strtab = true;
        return entry.data;
    }

    Result<const optional<elf::symtab_info>&, internal_error> elf::get_symtab() {
        if(did_load_symtab) {
            return symtab;
        }
        if(tried_to_load_symtab) {
            return internal_error("previous symtab load failed {}", file->path());
        }
        tried_to_load_symtab = true;
        if(is_64) {
            auto res = get_symtab_impl<64>(false);
            if(res.has_value()) {
                symtab = std::move(res).unwrap_value();
                did_load_symtab = true;
                return symtab;
            } else {
                return std::move(res).unwrap_error();
            }
        } else {
            auto res = get_symtab_impl<32>(false);
            if(res.has_value()) {
                symtab = std::move(res).unwrap_value();
                did_load_symtab = true;
                return symtab;
            } else {
                return std::move(res).unwrap_error();
            }
        }
    }

    Result<const optional<elf::symtab_info>&, internal_error> elf::get_dynamic_symtab() {
        if(did_load_dynamic_symtab) {
            return dynamic_symtab;
        }
        if(tried_to_load_dynamic_symtab) {
            return internal_error("previous dynamic symtab load failed {}", file->path());
        }
        tried_to_load_dynamic_symtab = true;
        if(is_64) {
            auto res = get_symtab_impl<64>(true);
            if(res.has_value()) {
                dynamic_symtab = std::move(res).unwrap_value();
                did_load_dynamic_symtab = true;
                return dynamic_symtab;
            } else {
                return std::move(res).unwrap_error();
            }
        } else {
            auto res = get_symtab_impl<32>(true);
            if(res.has_value()) {
                dynamic_symtab = std::move(res).unwrap_value();
                did_load_dynamic_symtab = true;
                return dynamic_symtab;
            } else {
                return std::move(res).unwrap_error();
            }
        }
    }

    template<std::size_t Bits>
    Result<optional<elf::symtab_info>, internal_error> elf::get_symtab_impl(bool dynamic) {
        // https://refspecs.linuxfoundation.org/elf/elf.pdf
        // page 66: only one sht_symtab and sht_dynsym section per file
        // page 32: symtab spec
        static_assert(Bits == 32 || Bits == 64, "Unexpected Bits argument");
        using SymEntry = typename std::conditional<Bits == 32, Elf32_Sym, Elf64_Sym>::type;
        auto sections_ = get_sections();
        if(sections_.is_error()) {
            return std::move(sections_).unwrap_error();
        }
        const auto& sections = sections_.unwrap_value();
        optional<symtab_info> symbol_table;
        for(const auto& section : sections) {
            if(section.sh_type == (dynamic ? SHT_DYNSYM : SHT_SYMTAB)) {
                if(section.sh_entsize != sizeof(SymEntry)) {
                    return internal_error("elf seems corrupted, sym entry mismatch {}", file->path());
                }
                if(section.sh_size % section.sh_entsize != 0) {
                    return internal_error("elf seems corrupted, sym entry vs section size mismatch {}", file->path());
                }
                std::vector<SymEntry> buffer(section.sh_size / section.sh_entsize);
                auto res = file->read_span(make_span(buffer.begin(), buffer.end()), section.sh_offset);
                if(!res) {
                    return res.unwrap_error();
                }
                symbol_table = symtab_info{};
                symbol_table.unwrap().entries.reserve(buffer.size());
                for(const auto& entry : buffer) {
                    symtab_entry normalized;
                    normalized.st_name = byteswap_if_needed(entry.st_name);
                    normalized.st_info = byteswap_if_needed(entry.st_info);
                    normalized.st_other = byteswap_if_needed(entry.st_other);
                    normalized.st_shndx = byteswap_if_needed(entry.st_shndx);
                    normalized.st_value = byteswap_if_needed(entry.st_value);
                    normalized.st_size = byteswap_if_needed(entry.st_size);
                    // on arm I've observed zero-size symbols that overlap with symbols we care about
                    // this interferes with some symbol lookup - that could be fixed by enhancing the logic there but
                    // also it's easy to just exclude zero-size symbols here
                    //  1413: 00000000000349e0     0 NOTYPE  LOCAL  DEFAULT   13 $x
                    // 32341: 00000000000349e0   220 FUNC    GLOBAL DEFAULT   13 _Z33stacktrace_from_current_rethrow_3RSt6vectorIiSaIiEE
                    if(normalized.st_size != 0) {
                        symbol_table.unwrap().entries.push_back(normalized);
                    }
                }
                std::sort(
                    symbol_table.unwrap().entries.begin(),
                    symbol_table.unwrap().entries.end(),
                    [] (const symtab_entry& a, const symtab_entry& b) {
                        return a.st_value < b.st_value;
                    }
                );
                symbol_table.unwrap().strtab_link = section.sh_link;
                break;
            }
        }
        return symbol_table;
    }

    Result<maybe_owned<elf>, internal_error> open_elf_cached(const std::string& object_path) {
        if(object_path.empty()) {
            return internal_error{"empty object_path"};
        }
        if(get_cache_mode() == cache_mode::prioritize_memory) {
            return elf::open(object_path)
                .transform([](elf&& obj) { return maybe_owned<elf>{detail::make_unique<elf>(std::move(obj))}; });
        } else {
            static std::mutex m;
            std::unique_lock<std::mutex> lock{m};
            // TODO: Re-evaluate storing the error
            static std::unordered_map<std::string, Result<elf, internal_error>> cache;
            auto it = cache.find(object_path);
            if(it == cache.end()) {
                auto res = cache.emplace(object_path, elf::open(object_path));
                VERIFY(res.second);
                it = res.first;
            }
            return it->second.transform([](elf& obj) { return maybe_owned<elf>(&obj); });
        }
    }
}
CPPTRACE_END_NAMESPACE

#endif
