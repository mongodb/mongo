#ifndef BOOST_DESCRIBE_DETAIL_BASES_HPP_INCLUDED
#define BOOST_DESCRIBE_DETAIL_BASES_HPP_INCLUDED

// Copyright 2020 Peter Dimov
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/describe/detail/compute_base_modifiers.hpp>
#include <boost/describe/detail/pp_for_each.hpp>
#include <boost/describe/detail/list.hpp>
#include <type_traits>

namespace boost
{
namespace describe
{
namespace detail
{

// base_descriptor
template<class C, class B> struct base_descriptor
{
    static_assert( std::is_base_of<B, C>::value, "A type listed as a base is not one" );

    using type = B;
    static constexpr unsigned modifiers = compute_base_modifiers<C, B>();
};

#ifndef __cpp_inline_variables
template<class C, class B> constexpr unsigned base_descriptor<C, B>::modifiers;
#endif

// bases_descriptor
template<typename ...>
struct bases_descriptor_impl;

template<class C, class ...Bs>
struct bases_descriptor_impl<C, list<Bs...>>
{
    using type = list<base_descriptor<C, Bs>...>;
};

#define BOOST_DESCRIBE_BASES(C, ...) inline auto boost_base_descriptor_fn( C** ) \
{ return typename boost::describe::detail::bases_descriptor_impl<C, boost::describe::detail::list<__VA_ARGS__>>::type(); }

} // namespace detail
} // namespace describe
} // namespace boost

#endif // #ifndef BOOST_DESCRIBE_DETAIL_BASES_HPP_INCLUDED
