#ifndef DWARF_UTILS_HPP
#define DWARF_UTILS_HPP

#include <cpptrace/basic.hpp>
#include "symbols/dwarf/dwarf.hpp"  // has dwarf #includes
#include "utils/error.hpp"
#include "utils/microfmt.hpp"
#include "utils/utils.hpp"

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
namespace libdwarf {
    class srcfiles {
        Dwarf_Debug dbg = nullptr;
        char** dw_srcfiles = nullptr;
        Dwarf_Unsigned dw_filecount = 0;

    public:
        srcfiles(Dwarf_Debug dbg, char** dw_srcfiles, Dwarf_Signed filecount)
            : dbg(dbg), dw_srcfiles(dw_srcfiles), dw_filecount(static_cast<Dwarf_Unsigned>(filecount))
        {
            if(filecount < 0) {
                throw internal_error("Unexpected dw_filecount {}", filecount);
            }
        }
        ~srcfiles() {
            release();
        }
        void release() {
            if(dw_srcfiles) {
                for(unsigned i = 0; i < dw_filecount; i++) {
                    dwarf_dealloc(dbg, dw_srcfiles[i], DW_DLA_STRING);
                    dw_srcfiles[i] = nullptr;
                }
                dwarf_dealloc(dbg, dw_srcfiles, DW_DLA_LIST);
                dw_srcfiles = nullptr;
            }
        }
        srcfiles(const srcfiles&) = delete;
        srcfiles(srcfiles&& other) {
            *this = std::move(other);
        }
        srcfiles& operator=(const srcfiles&) = delete;
        srcfiles& operator=(srcfiles&& other) {
            release();
            dbg = exchange(other.dbg, nullptr);
            dw_srcfiles = exchange(other.dw_srcfiles, nullptr);
            dw_filecount = exchange(other.dw_filecount, 0);
            return *this;
        }
        // note: dwarf uses 1-indexing
        const char* get(Dwarf_Unsigned file_i) const {
            if(file_i >= dw_filecount) {
                throw internal_error(
                    "Error while accessing the srcfiles list, requested index {} is out of bounds (count = {})",
                    file_i,
                    dw_filecount
                );
            }
            return dw_srcfiles[file_i];
        }
        Dwarf_Unsigned count() const {
            return dw_filecount;
        }
    };

    // container of items which are keyed by ranges
    template<typename K, typename V>
    class range_map {
    public:
        struct handle {
            std::uint32_t index;
        };
    private:
        struct PACKED range_entry {
            handle item;
            K low;
            K high;
        };
        std::vector<V> items;
        std::vector<range_entry> range_entries;
    public:
        handle add_item(V&& item) {
            items.push_back(std::move(item));
            VERIFY(items.size() < std::numeric_limits<std::uint32_t>::max());
            return handle{static_cast<std::uint32_t>(items.size() - 1)};
        }
        void insert(handle handle, K low, K high) {
            range_entries.push_back({handle, low, high});
        }
        void finalize() {
            std::sort(range_entries.begin(), range_entries.end(), [] (const range_entry& a, const range_entry& b) {
                return a.low < b.low;
            });
        }
        std::size_t ranges_count() const {
            return range_entries.size();
        }

        optional<const V&> lookup(K key) const {
            auto vec_it = first_less_than_or_equal(
                range_entries.begin(),
                range_entries.end(),
                key,
                [] (K key, const range_entry& entry) {
                    return key < entry.low;
                }
            );
            if(vec_it == range_entries.end()) {
                return nullopt;
            }
            return items.at(vec_it->item.index);
        }
    };

    struct line_entry {
        Dwarf_Addr low;
        // Dwarf_Addr high;
        // int i;
        Dwarf_Line line;
        optional<std::string> path;
        optional<std::uint32_t> line_number;
        optional<std::uint32_t> column_number;
        line_entry(Dwarf_Addr low, Dwarf_Line line) : low(low), line(line) {}
    };

    struct line_table_info {
        Dwarf_Unsigned version = 0;
        Dwarf_Line_Context line_context = nullptr;
        // sorted by low_addr
        // TODO: Make this optional at some point, it may not be generated if cache mode switches during program exec...
        std::vector<line_entry> line_entries;

        line_table_info(
            Dwarf_Unsigned version,
            Dwarf_Line_Context line_context,
            std::vector<line_entry>&& line_entries
        ) : version(version), line_context(line_context), line_entries(std::move(line_entries)) {}
        ~line_table_info() {
            release();
        }
        void release() {
            dwarf_srclines_dealloc_b(line_context);
            line_context = nullptr;
        }
        line_table_info(const line_table_info&) = delete;
        line_table_info(line_table_info&& other) {
            *this = std::move(other);
        }
        line_table_info& operator=(const line_table_info&) = delete;
        line_table_info& operator=(line_table_info&& other) {
            release();
            version = other.version;
            line_context = exchange(other.line_context, nullptr);
            line_entries = std::move(other.line_entries);
            return *this;
        }
    };
}
}
CPPTRACE_END_NAMESPACE

#endif
