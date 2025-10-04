#ifndef DWARF_OPTIONS_HPP
#define DWARF_OPTIONS_HPP

#include "utils/optional.hpp"

#include <cstddef>

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
    optional<std::size_t> get_dwarf_resolver_line_table_cache_size();
    bool get_dwarf_resolver_disable_aranges();
}
CPPTRACE_END_NAMESPACE

#endif
