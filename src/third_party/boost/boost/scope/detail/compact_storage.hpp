/*
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * https://www.boost.org/LICENSE_1_0.txt)
 *
 * Copyright (c) 2022 Andrey Semashev
 */
/*!
 * \file scope/detail/compact_storage.hpp
 *
 * This header contains utility helpers for implementing compact storage
 * for class members. In particular, it allows to leverage empty base optimization (EBO).
 */

#ifndef BOOST_SCOPE_DETAIL_COMPACT_STORAGE_HPP_INCLUDED_
#define BOOST_SCOPE_DETAIL_COMPACT_STORAGE_HPP_INCLUDED_

#include <type_traits>
#include <boost/scope/detail/config.hpp>
#include <boost/scope/detail/type_traits/is_final.hpp>
#include <boost/scope/detail/type_traits/negation.hpp>
#include <boost/scope/detail/type_traits/conjunction.hpp>
#include <boost/scope/detail/header.hpp>

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

namespace boost {
namespace scope {
namespace detail {

//! The class allows to place data members in the tail padding of type \a T if the user's class derives from it
template<
    typename T,
    typename Tag = void,
    bool = detail::conjunction< std::is_class< T >, detail::negation< detail::is_final< T > > >::value
>
class compact_storage :
    private T
{
public:
    template< typename... Args >
    constexpr compact_storage(Args&&... args) noexcept(std::is_nothrow_constructible< T, Args... >::value) :
        T(static_cast< Args&& >(args)...)
    {
    }

    compact_storage(compact_storage&&) = default;
    compact_storage& operator= (compact_storage&&) = default;

    compact_storage(compact_storage const&) = default;
    compact_storage& operator= (compact_storage const&) = default;

    T& get() noexcept
    {
        return *static_cast< T* >(this);
    }

    T const& get() const noexcept
    {
        return *static_cast< const T* >(this);
    }
};

template< typename T, typename Tag >
class compact_storage< T, Tag, false >
{
private:
    T m_data;

public:
    template< typename... Args >
    constexpr compact_storage(Args&&... args) noexcept(std::is_nothrow_constructible< T, Args... >::value) :
        m_data(static_cast< Args&& >(args)...)
    {
    }

    compact_storage(compact_storage&&) = default;
    compact_storage& operator= (compact_storage&&) = default;

    compact_storage(compact_storage const&) = default;
    compact_storage& operator= (compact_storage const&) = default;

    T& get() noexcept
    {
        return m_data;
    }

    T const& get() const noexcept
    {
        return m_data;
    }
};

} // namespace detail
} // namespace scope
} // namespace boost

#include <boost/scope/detail/footer.hpp>

#endif // BOOST_SCOPE_DETAIL_COMPACT_STORAGE_HPP_INCLUDED_
