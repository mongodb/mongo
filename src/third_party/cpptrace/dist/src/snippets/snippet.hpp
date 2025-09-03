#ifndef SNIPPET_HPP
#define SNIPPET_HPP

#include <cstddef>
#include <string>

#include <cpptrace/basic.hpp>

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
    // 1-indexed line
    std::string get_snippet(
        const std::string& path,
        std::size_t line,
        nullable<std::uint32_t> column,
        std::size_t context_size,
        bool color
    );
}
CPPTRACE_END_NAMESPACE

#endif
