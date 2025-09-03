#ifndef DWARF_HPP
#define DWARF_HPP

#include <cpptrace/basic.hpp>
#include "utils/error.hpp"
#include "utils/microfmt.hpp"
#include "utils/utils.hpp"

#include <functional>
#include <type_traits>

#ifdef CPPTRACE_USE_NESTED_LIBDWARF_HEADER_PATH
 #include <libdwarf/libdwarf.h>
 #include <libdwarf/dwarf.h>
#else
 #include <libdwarf.h>
 #include <dwarf.h>
#endif

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
namespace libdwarf {
    static_assert(std::is_pointer<Dwarf_Die>::value, "Dwarf_Die not a pointer");
    static_assert(std::is_pointer<Dwarf_Debug>::value, "Dwarf_Debug not a pointer");

    using rangelist_entries = std::vector<std::pair<Dwarf_Addr, Dwarf_Addr>>;

    [[noreturn]] inline void handle_dwarf_error(Dwarf_Debug dbg, Dwarf_Error error) {
        Dwarf_Unsigned ev = dwarf_errno(error);
        // dwarf_dealloc_error deallocates the message, attaching to msg is convenient
        auto msg = raii_wrap(dwarf_errmsg(error), [dbg, error] (char*) { dwarf_dealloc_error(dbg, error); });
        throw internal_error("dwarf error {} {}", ev, msg.get());
    }

    struct die_object {
        Dwarf_Debug dbg = nullptr;
        Dwarf_Die die = nullptr;

        // Error handling helper
        // For some reason R (*f)(Args..., void*)-style deduction isn't possible, seems like a bug in all compilers
        // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=56190
        template<
            typename... Args,
            typename... Args2,
            typename std::enable_if<
                std::is_same<
                    decltype(
                        (void)std::declval<int(Args...)>()(std::forward<Args2>(std::declval<Args2>())..., nullptr)
                    ),
                    void
                >::value,
                int
            >::type = 0
        >
        int wrap(int (*f)(Args...), Args2&&... args) const {
            Dwarf_Error error = nullptr;
            int ret = f(std::forward<Args2>(args)..., &error);
            if(ret == DW_DLV_ERROR) {
                handle_dwarf_error(dbg, error);
            }
            return ret;
        }

        die_object(Dwarf_Debug dbg, Dwarf_Die die) : dbg(dbg), die(die) {
            ASSERT(dbg != nullptr);
        }

        ~die_object() {
            release();
        }

        void release() {
            if(die) {
                dwarf_dealloc_die(exchange(die, nullptr));
            }
        }

        die_object(const die_object&) = delete;

        die_object& operator=(const die_object&) = delete;

        // dbg doesn't strictly have to be st to null but it helps ensure attempts to use the die_object after this to
        // segfault. A valid use otherwise would be moved_from.get_sibling() which would get the next CU.
        die_object(die_object&& other) noexcept
            : dbg(exchange(other.dbg, nullptr)), die(exchange(other.die, nullptr)) {}

        die_object& operator=(die_object&& other) noexcept {
            release();
            dbg = exchange(other.dbg, nullptr);
            die = exchange(other.die, nullptr);
            return *this;
        }

        die_object clone() const {
            Dwarf_Off global_offset = get_global_offset();
            Dwarf_Bool is_info = dwarf_get_die_infotypes_flag(die);
            Dwarf_Die die_copy = nullptr;
            VERIFY(wrap(dwarf_offdie_b, dbg, global_offset, is_info, &die_copy) == DW_DLV_OK);
            return {dbg, die_copy};
        }

        die_object get_child() const {
            Dwarf_Die child = nullptr;
            int ret = wrap(dwarf_child, die, &child);
            if(ret == DW_DLV_OK) {
                return die_object(dbg, child);
            } else if(ret == DW_DLV_NO_ENTRY) {
                return die_object(dbg, nullptr);
            } else {
                PANIC();
            }
        }

        die_object get_sibling() const {
            Dwarf_Die sibling = nullptr;
            int ret = wrap(dwarf_siblingof_b, dbg, die, true, &sibling);
            if(ret == DW_DLV_OK) {
                return die_object(dbg, sibling);
            } else if(ret == DW_DLV_NO_ENTRY) {
                return die_object(dbg, nullptr);
            } else {
                PANIC();
            }
        }

        operator bool() const {
            return die != nullptr;
        }

        Dwarf_Die get() const {
            return die;
        }

        std::string get_name() const {
            char empty[] = "";
            char* name = empty;
            // Note: It's important to not free the string from this function.
            int ret = wrap(dwarf_diename, die, &name);
            std::string str;
            if(ret != DW_DLV_NO_ENTRY) {
                str = name;
            }
            return name;
        }

        optional<std::string> get_string_attribute(Dwarf_Half attr_num) const {
            Dwarf_Attribute attr;
            if(wrap(dwarf_attr, die, attr_num, &attr) == DW_DLV_OK) {
                auto attwrapper = raii_wrap(attr, [] (Dwarf_Attribute attr) { dwarf_dealloc_attribute(attr); });
                char* raw_str;
                // Note: It's important to not free the string from this function.
                // https://github.com/davea42/libdwarf-code/issues/279
                VERIFY(wrap(dwarf_formstring, attr, &raw_str) == DW_DLV_OK);
                std::string str = raw_str;
                return str;
            } else {
                return nullopt;
            }
        }

        optional<Dwarf_Unsigned> get_unsigned_attribute(Dwarf_Half attr_num) const {
            Dwarf_Attribute attr;
            if(wrap(dwarf_attr, die, attr_num, &attr) == DW_DLV_OK) {
                auto attwrapper = raii_wrap(attr, [] (Dwarf_Attribute attr) { dwarf_dealloc_attribute(attr); });
                // Dwarf_Half form = 0;
                // VERIFY(wrap(dwarf_whatform, attr, &form) == DW_DLV_OK);
                Dwarf_Unsigned val;
                VERIFY(wrap(dwarf_formudata, attr, &val) == DW_DLV_OK);
                return val;
            } else {
                return nullopt;
            }
        }

        bool has_attr(Dwarf_Half attr_num) const {
            Dwarf_Bool present = false;
            VERIFY(wrap(dwarf_hasattr, die, attr_num, &present) == DW_DLV_OK);
            return present;
        }

        Dwarf_Half get_tag() const {
            Dwarf_Half tag = 0;
            VERIFY(wrap(dwarf_tag, die, &tag) == DW_DLV_OK);
            return tag;
        }

        const char* get_tag_name() const {
            const char* tag_name;
            if(dwarf_get_TAG_name(get_tag(), &tag_name) == DW_DLV_OK) {
                return tag_name;
            } else {
                return "<unknown tag name>";
            }
        }

        Dwarf_Off get_global_offset() const {
            Dwarf_Off off;
            VERIFY(wrap(dwarf_dieoffset, die, &off) == DW_DLV_OK);
            return off;
        }

        die_object resolve_reference_attribute(Dwarf_Half attr_num) const {
            Dwarf_Attribute attr;
            VERIFY(dwarf_attr(die, attr_num, &attr, nullptr) == DW_DLV_OK);
            auto wrapper = raii_wrap(attr, [] (Dwarf_Attribute attr) { dwarf_dealloc_attribute(attr); });
            Dwarf_Half form = 0;
            VERIFY(wrap(dwarf_whatform, attr, &form) == DW_DLV_OK);
            switch(form) {
                case DW_FORM_ref1:
                case DW_FORM_ref2:
                case DW_FORM_ref4:
                case DW_FORM_ref8:
                case DW_FORM_ref_udata:
                    {
                        Dwarf_Off off = 0;
                        Dwarf_Bool is_info = dwarf_get_die_infotypes_flag(die);
                        VERIFY(wrap(dwarf_formref, attr, &off, &is_info) == DW_DLV_OK);
                        Dwarf_Off global_offset = 0;
                        VERIFY(wrap(dwarf_convert_to_global_offset, attr, off, &global_offset) == DW_DLV_OK);
                        Dwarf_Die target = nullptr;
                        VERIFY(wrap(dwarf_offdie_b, dbg, global_offset, is_info, &target) == DW_DLV_OK);
                        return die_object(dbg, target);
                    }
                case DW_FORM_ref_addr:
                    {
                        Dwarf_Off off;
                        VERIFY(wrap(dwarf_global_formref, attr, &off) == DW_DLV_OK);
                        int is_info = dwarf_get_die_infotypes_flag(die);
                        Dwarf_Die target = nullptr;
                        VERIFY(wrap(dwarf_offdie_b, dbg, off, is_info, &target) == DW_DLV_OK);
                        return die_object(dbg, target);
                    }
                case DW_FORM_ref_sig8:
                    {
                        Dwarf_Sig8 signature;
                        VERIFY(wrap(dwarf_formsig8, attr, &signature) == DW_DLV_OK);
                        Dwarf_Die target = nullptr;
                        Dwarf_Bool targ_is_info = false;
                        VERIFY(wrap(dwarf_find_die_given_sig8, dbg, &signature, &target, &targ_is_info) == DW_DLV_OK);
                        return die_object(dbg, target);
                    }
                default:
                    PANIC(microfmt::format("unknown form for attribute {} {}\n", attr_num, form));
            }
        }

        Dwarf_Unsigned get_ranges_base_address(const die_object& cu_die) const {
            // After libdwarf v0.11.0 this can use dwarf_get_ranges_baseaddress, however, in the interest of not
            // requiring v0.11.0 just yet the logic is implemented here too.
            // The base address is:
            // - If the die has a rangelist, use the low_pc for that die
            // - Otherwise use the low_pc from the CU if present
            // - Otherwise 0
            if(has_attr(DW_AT_ranges)) {
                if(has_attr(DW_AT_low_pc)) {
                    Dwarf_Addr lowpc;
                    if(wrap(dwarf_lowpc, die, &lowpc) == DW_DLV_OK) {
                        return lowpc;
                    }
                }
            }
            if(cu_die.has_attr(DW_AT_low_pc)) {
                Dwarf_Addr lowpc;
                if(wrap(dwarf_lowpc, cu_die.get(), &lowpc) == DW_DLV_OK) {
                    return lowpc;
                }
            }
            return 0;
        }

        Dwarf_Unsigned get_ranges_offset(Dwarf_Attribute attr) const {
            Dwarf_Unsigned off = 0;
            Dwarf_Half form = 0;
            VERIFY(wrap(dwarf_whatform, attr, &form) == DW_DLV_OK);
            if (form == DW_FORM_rnglistx) {
                VERIFY(wrap(dwarf_formudata, attr, &off) == DW_DLV_OK);
            } else {
                VERIFY(wrap(dwarf_global_formref, attr, &off) == DW_DLV_OK);
            }
            return off;
        }

        template<typename F>
        // callback should return true to keep going
        void dwarf5_ranges(F callback) const {
            Dwarf_Attribute attr = nullptr;
            if(wrap(dwarf_attr, die, DW_AT_ranges, &attr) != DW_DLV_OK) {
                return;
            }
            auto attrwrapper = raii_wrap(attr, [] (Dwarf_Attribute attr) { dwarf_dealloc_attribute(attr); });
            Dwarf_Unsigned offset = get_ranges_offset(attr);
            Dwarf_Half form = 0;
            VERIFY(wrap(dwarf_whatform, attr, &form) == DW_DLV_OK);
            // get .debug_rnglists info
            Dwarf_Rnglists_Head head = nullptr;
            Dwarf_Unsigned rnglists_entries = 0;
            Dwarf_Unsigned dw_global_offset_of_rle_set = 0;
            int res = wrap(
                dwarf_rnglists_get_rle_head,
                attr,
                form,
                offset,
                &head,
                &rnglists_entries,
                &dw_global_offset_of_rle_set
            );
            auto headwrapper = raii_wrap(head, [] (Dwarf_Rnglists_Head head) { dwarf_dealloc_rnglists_head(head); });
            if(res == DW_DLV_NO_ENTRY) {
                return;
            }
            VERIFY(res == DW_DLV_OK);
            for(std::size_t i = 0 ; i < rnglists_entries; i++) {
                unsigned entrylen = 0;
                unsigned rle_value_out = 0;
                Dwarf_Unsigned raw1 = 0;
                Dwarf_Unsigned raw2 = 0;
                Dwarf_Bool unavailable = 0;
                Dwarf_Unsigned cooked1 = 0;
                Dwarf_Unsigned cooked2 = 0;
                res = wrap(
                    dwarf_get_rnglists_entry_fields_a,
                    head,
                    i,
                    &entrylen,
                    &rle_value_out,
                    &raw1,
                    &raw2,
                    &unavailable,
                    &cooked1,
                    &cooked2
                );
                if(res == DW_DLV_NO_ENTRY) {
                    continue;
                }
                VERIFY(res == DW_DLV_OK);
                if(unavailable) {
                    continue;
                }
                switch(rle_value_out) {
                    // Following the same scheme from libdwarf-addr2line
                    case DW_RLE_end_of_list:
                    case DW_RLE_base_address:
                    case DW_RLE_base_addressx:
                        // Already handled
                        break;
                    case DW_RLE_offset_pair:
                    case DW_RLE_startx_endx:
                    case DW_RLE_start_end:
                    case DW_RLE_startx_length:
                    case DW_RLE_start_length:
                        if(!callback(cooked1, cooked2)) {
                            return;
                        }
                        break;
                    default:
                        PANIC("Something is wrong");
                        break;
                }
            }
        }

        template<typename F>
        // callback should return true to keep going
        void dwarf4_ranges(Dwarf_Addr baseaddr, F callback) const {
            Dwarf_Attribute attr = nullptr;
            if(wrap(dwarf_attr, die, DW_AT_ranges, &attr) != DW_DLV_OK) {
                return;
            }
            auto attrwrapper = raii_wrap(attr, [] (Dwarf_Attribute attr) { dwarf_dealloc_attribute(attr); });
            Dwarf_Unsigned offset;
            if(wrap(dwarf_global_formref, attr, &offset) != DW_DLV_OK) {
                return;
            }
            Dwarf_Addr baseaddr_original = baseaddr;
            Dwarf_Ranges* ranges = nullptr;
            Dwarf_Signed count = 0;
            VERIFY(
                wrap(
                    dwarf_get_ranges_b,
                    dbg,
                    offset,
                    die,
                    nullptr,
                    &ranges,
                    &count,
                    nullptr
                ) == DW_DLV_OK
            );
            auto rangeswrapper = raii_wrap(
                ranges,
                [this, count] (Dwarf_Ranges* ranges) { dwarf_dealloc_ranges(dbg, ranges, count); }
            );
            for(int i = 0; i < count; i++) {
                if(ranges[i].dwr_type == DW_RANGES_ENTRY) {
                    if(!callback(baseaddr + ranges[i].dwr_addr1, baseaddr + ranges[i].dwr_addr2)) {
                        return;
                    }
                } else if(ranges[i].dwr_type == DW_RANGES_ADDRESS_SELECTION) {
                    baseaddr = ranges[i].dwr_addr2;
                } else {
                    ASSERT(ranges[i].dwr_type == DW_RANGES_END);
                    baseaddr = baseaddr_original;
                }
            }
        }

        template<typename F>
        // callback should return true to keep going
        void dwarf_ranges(const die_object& cu_die, int version, F callback) const {
            Dwarf_Addr lowpc;
            if(wrap(dwarf_lowpc, die, &lowpc) == DW_DLV_OK) {
                Dwarf_Addr highpc = 0;
                enum Dwarf_Form_Class return_class;
                if(wrap(dwarf_highpc_b, die, &highpc, nullptr, &return_class) == DW_DLV_OK) {
                    if(return_class == DW_FORM_CLASS_CONSTANT) {
                        highpc += lowpc;
                    }
                    if(!callback(lowpc, highpc)) {
                        return;
                    }
                }
            }
            if(version >= 5) {
                dwarf5_ranges(callback);
            } else {
                dwarf4_ranges(get_ranges_base_address(cu_die), callback);
            }
        }

        rangelist_entries get_rangelist_entries(const die_object& cu_die, int version) const {
            rangelist_entries vec;
            dwarf_ranges(cu_die, version, [&vec] (Dwarf_Addr low, Dwarf_Addr high) {
                // Simple coalescing optimization:
                // Sometimes the range list entries are really continuous: [100, 200), [200, 300)
                // Other times there's just one byte of separation [300, 399), [400, 500)
                // Those are the main two cases I've observed.
                // This will not catch all cases, presumably, as the range lists aren't sorted. But compilers/linkers
                // seem to like to emit the ranges in sorted order.
                if(!vec.empty() && low - vec.back().second <= 1) {
                    vec.back().second = high;
                } else {
                    vec.push_back({low, high});
                }
                return true;
            });
            return vec;
        }

        Dwarf_Bool pc_in_die(const die_object& cu_die, int version, Dwarf_Addr pc) const {
            bool found = false;
            dwarf_ranges(cu_die, version, [&found, pc] (Dwarf_Addr low, Dwarf_Addr high) {
                if(pc >= low && pc < high) {
                    found = true;
                    return false;
                }
                return true;
            });
            return found;
        }

        void print() const {
            std::fprintf(
                stderr,
                "%08llx %s %s\n",
                to_ull(get_global_offset()),
                get_tag_name(),
                get_name().c_str()
            );
        }
    };

    // walk die list, callback is called on each die and should return true to
    // continue traversal
    // returns true if traversal should continue
    inline bool walk_die_list(
        const die_object& die,
        const std::function<bool(const die_object&)>& fn
    ) {
        // TODO: Refactor so there is only one fn call
        bool continue_traversal = true;
        if(fn(die)) {
            die_object current = die.get_sibling();
            while(current) {
                if(fn(current)) {
                    current = current.get_sibling();
                } else {
                    continue_traversal = false;
                    break;
                }
            }
        }
        return continue_traversal;
    }

    // walk die list, recursing into children, callback is called on each die
    // and should return true to continue traversal
    // returns true if traversal should continue
    inline bool walk_die_list_recursive(
        const die_object& die,
        const std::function<bool(const die_object&)>& fn
    ) {
        return walk_die_list(
            die,
            [&fn](const die_object& die) {
                auto child = die.get_child();
                if(child) {
                    if(!walk_die_list_recursive(child, fn)) {
                        return false;
                    }
                }
                return fn(die);
            }
        );
    }

    class maybe_owned_die_object {
        // Hacky... I wish std::variant existed.
        optional<die_object> owned_die;
        optional<const die_object*> ref_die;
        maybe_owned_die_object(die_object&& die) : owned_die(std::move(die)) {}
        maybe_owned_die_object(const die_object& die) : ref_die(&die) {}
    public:
        static maybe_owned_die_object owned(die_object&& die) {
            return maybe_owned_die_object{std::move(die)};
        }
        static maybe_owned_die_object ref(const die_object& die) {
            return maybe_owned_die_object{die};
        }
        const die_object& get() {
            ASSERT(owned_die || ref_die, "Mal-formed maybe_owned_die_object");
            if(owned_die) {
                return owned_die.unwrap();
            } else {
                return *ref_die.unwrap();
            }
        }
    };
}
}
CPPTRACE_END_NAMESPACE

#endif
