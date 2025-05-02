/*
 * Copyright Matt Borland 2025.
 * Distributed under the Boost Software License, Version 1.0. (See
        * accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See http://www.boost.org for most recent version including documentation.
 *
 * $Id$
 */

#include <iterator>
#include <cstddef>

namespace boost {
namespace random {
namespace detail {

#if defined (__cpp_lib_nonmember_container_access) && __cpp_lib_nonmember_container_access >= 201411L

using std::size;

#else

template <typename C>
constexpr auto size(const C& c) -> decltype(c.size())
{
    return c.size();
}

template <typename T, std::size_t N>
constexpr std::size_t size(const T (&)[N]) noexcept
{
    return N;
}

#endif

} // namespace detail
} // namespace random
} // namespace boost
