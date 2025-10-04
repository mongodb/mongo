//
// Copyright (c) 2024 Alexander Grund
//
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#ifndef BOOST_LOCALE_DETAIL_ALLOCATOR_TRAITS_HPP_INCLUDED
#define BOOST_LOCALE_DETAIL_ALLOCATOR_TRAITS_HPP_INCLUDED

#include <boost/locale/config.hpp>
#include <memory>
#include <type_traits>

/// \cond INTERNAL
namespace boost { namespace locale { namespace conv { namespace detail {
    template<class Alloc, typename T>
    using rebind_alloc = typename std::allocator_traits<Alloc>::template rebind_alloc<T>;

    template<class Alloc, typename T, typename Result = void>
    using enable_if_allocator_for =
      typename std::enable_if<std::is_same<typename Alloc::value_type, T>::value, Result>::type;
    template<class Alloc, typename T, class Alloc2, typename T2, typename Result = void>
    using enable_if_allocator_for2 = typename std::enable_if<std::is_same<typename Alloc::value_type, T>::value
                                                               && std::is_same<typename Alloc2::value_type, T2>::value,
                                                             Result>::type;
}}}} // namespace boost::locale::conv::detail

/// \endcond

#endif
