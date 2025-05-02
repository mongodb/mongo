/* Hash function characterization.
 *
 * Copyright 2022-2024 Joaquin M Lopez Munoz.
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * See https://www.boost.org/libs/unordered for library home page.
 */

#ifndef BOOST_UNORDERED_HASH_TRAITS_HPP
#define BOOST_UNORDERED_HASH_TRAITS_HPP

#include <boost/unordered/detail/type_traits.hpp>

namespace boost{
namespace unordered{

namespace detail{

template<typename Hash,typename=void>
struct hash_is_avalanching_impl:std::false_type{};

template<typename IsAvalanching>
struct avalanching_value
{
  static constexpr bool value=IsAvalanching::value;
};

/* may be explicitly marked as BOOST_DEPRECATED in the future */
template<> struct avalanching_value<void>
{
  static constexpr bool value=true;
};

template<typename Hash>
struct hash_is_avalanching_impl<
  Hash,
  boost::unordered::detail::void_t<typename Hash::is_avalanching>
>:std::integral_constant<
  bool,
  avalanching_value<typename Hash::is_avalanching>::value
>{};

template<typename Hash>
struct hash_is_avalanching_impl<
  Hash,
  typename std::enable_if<((void)Hash::is_avalanching,true)>::type
>{}; /* Hash::is_avalanching is not a type: compile error downstream */

} /* namespace detail */

/* Each trait can be partially specialized by users for concrete hash functions
 * when actual characterization differs from default.
 */

/* hash_is_avalanching<Hash>::value is:
 *   - false if Hash::is_avalanching is not present.
 *   - Hash::is_avalanching::value if this is present and constexpr-convertible
 *     to a bool.
 *   - true if Hash::is_avalanching is void (deprecated).
 *   - ill-formed otherwise.
 */
template<typename Hash>
struct hash_is_avalanching: detail::hash_is_avalanching_impl<Hash>::type{};

} /* namespace unordered */
} /* namespace boost */

#endif
