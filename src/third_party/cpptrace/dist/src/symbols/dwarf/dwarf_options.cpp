#include "symbols/dwarf/dwarf_options.hpp"

#include <cpptrace/utils.hpp>

#include <atomic>

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
    std::atomic<nullable<std::size_t>> dwarf_resolver_line_table_cache_size{nullable<std::size_t>::null()};
    std::atomic<bool> dwarf_resolver_disable_aranges{false};

    optional<std::size_t> get_dwarf_resolver_line_table_cache_size() {
        auto max_entries = dwarf_resolver_line_table_cache_size.load();
        return max_entries.has_value() ? optional<std::size_t>(max_entries.value()) : nullopt;
    }

    bool get_dwarf_resolver_disable_aranges() {
        return dwarf_resolver_disable_aranges.load();
    }
}
CPPTRACE_END_NAMESPACE

CPPTRACE_BEGIN_NAMESPACE
namespace experimental {
    void set_dwarf_resolver_line_table_cache_size(nullable<std::size_t> max_entries) {
        detail::dwarf_resolver_line_table_cache_size.store(max_entries);
    }

    void set_dwarf_resolver_disable_aranges(bool disable) {
        detail::dwarf_resolver_disable_aranges.store(disable);
    }
}
CPPTRACE_END_NAMESPACE
