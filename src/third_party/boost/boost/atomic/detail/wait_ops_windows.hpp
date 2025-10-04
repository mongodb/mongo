/*
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * Copyright (c) 2020 Andrey Semashev
 */
/*!
 * \file   atomic/detail/wait_ops_windows.hpp
 *
 * This header contains implementation of the waiting/notifying atomic operations on Windows.
 */

#ifndef BOOST_ATOMIC_DETAIL_WAIT_OPS_WINDOWS_HPP_INCLUDED_
#define BOOST_ATOMIC_DETAIL_WAIT_OPS_WINDOWS_HPP_INCLUDED_

#include <boost/atomic/detail/config.hpp>
#include <boost/memory_order.hpp>
#include <boost/winapi/wait_constants.hpp>
#include <boost/atomic/detail/wait_operations_fwd.hpp>
#include <boost/atomic/detail/wait_capabilities.hpp>
#if defined(BOOST_ATOMIC_DETAIL_WINDOWS_HAS_WAIT_ON_ADDRESS)
#include <boost/winapi/wait_on_address.hpp>
#if (defined(BOOST_ATOMIC_FORCE_AUTO_LINK) || (!defined(BOOST_ALL_NO_LIB) && !defined(BOOST_ATOMIC_NO_LIB))) && !defined(BOOST_ATOMIC_NO_SYNCHRONIZATION_LIB)
#define BOOST_LIB_NAME "synchronization"
#if defined(BOOST_AUTO_LINK_NOMANGLE)
#include <boost/config/auto_link.hpp>
#else // defined(BOOST_AUTO_LINK_NOMANGLE)
#define BOOST_AUTO_LINK_NOMANGLE
#include <boost/config/auto_link.hpp>
#undef BOOST_AUTO_LINK_NOMANGLE
#endif // defined(BOOST_AUTO_LINK_NOMANGLE)
#endif // (defined(BOOST_ATOMIC_FORCE_AUTO_LINK) || (!defined(BOOST_ALL_NO_LIB) && !defined(BOOST_ATOMIC_NO_LIB))) && !defined(BOOST_ATOMIC_NO_SYNCHRONIZATION_LIB)
#else // defined(BOOST_ATOMIC_DETAIL_WINDOWS_HAS_WAIT_ON_ADDRESS)
#include <cstddef>
#include <boost/atomic/detail/wait_on_address.hpp>
#include <boost/atomic/detail/wait_ops_generic.hpp>
#endif // defined(BOOST_ATOMIC_DETAIL_WINDOWS_HAS_WAIT_ON_ADDRESS)
#include <boost/atomic/detail/header.hpp>

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

namespace boost {
namespace atomics {
namespace detail {

#if defined(BOOST_ATOMIC_DETAIL_WINDOWS_HAS_WAIT_ON_ADDRESS)

template< typename Base, std::size_t Size >
struct wait_operations_windows :
    public Base
{
    typedef Base base_type;
    typedef typename base_type::storage_type storage_type;

    static BOOST_CONSTEXPR_OR_CONST bool always_has_native_wait_notify = true;

    static BOOST_FORCEINLINE bool has_native_wait_notify(storage_type const volatile&) BOOST_NOEXCEPT
    {
        return true;
    }

    static BOOST_FORCEINLINE storage_type wait(storage_type const volatile& storage, storage_type old_val, memory_order order) BOOST_NOEXCEPT
    {
        storage_type new_val = base_type::load(storage, order);
        while (new_val == old_val)
        {
            boost::winapi::WaitOnAddress(const_cast< storage_type* >(&storage), &old_val, Size, boost::winapi::infinite);
            new_val = base_type::load(storage, order);
        }

        return new_val;
    }

    static BOOST_FORCEINLINE void notify_one(storage_type volatile& storage) BOOST_NOEXCEPT
    {
        boost::winapi::WakeByAddressSingle(const_cast< storage_type* >(&storage));
    }

    static BOOST_FORCEINLINE void notify_all(storage_type volatile& storage) BOOST_NOEXCEPT
    {
        boost::winapi::WakeByAddressAll(const_cast< storage_type* >(&storage));
    }
};

#else // defined(BOOST_ATOMIC_DETAIL_WINDOWS_HAS_WAIT_ON_ADDRESS)

template< typename Base, std::size_t Size >
struct wait_operations_windows :
    public atomics::detail::wait_operations_generic< Base, false >
{
    typedef atomics::detail::wait_operations_generic< Base, false > base_type;
    typedef typename base_type::storage_type storage_type;

    static BOOST_CONSTEXPR_OR_CONST bool always_has_native_wait_notify = false;

    static BOOST_FORCEINLINE bool has_native_wait_notify(storage_type const volatile&) BOOST_NOEXCEPT
    {
        ensure_wait_functions_initialized();
        return atomics::detail::wait_on_address != NULL;
    }

    static BOOST_FORCEINLINE storage_type wait(storage_type const volatile& storage, storage_type old_val, memory_order order) BOOST_NOEXCEPT
    {
        ensure_wait_functions_initialized();

        if (BOOST_LIKELY(atomics::detail::wait_on_address != NULL))
        {
            storage_type new_val = base_type::load(storage, order);
            while (new_val == old_val)
            {
                atomics::detail::wait_on_address(const_cast< storage_type* >(&storage), &old_val, Size, boost::winapi::infinite);
                new_val = base_type::load(storage, order);
            }

            return new_val;
        }
        else
        {
            return base_type::wait(storage, old_val, order);
        }
    }

    static BOOST_FORCEINLINE void notify_one(storage_type volatile& storage) BOOST_NOEXCEPT
    {
        ensure_wait_functions_initialized();

        if (BOOST_LIKELY(atomics::detail::wake_by_address_single != NULL))
            atomics::detail::wake_by_address_single(const_cast< storage_type* >(&storage));
        else
            base_type::notify_one(storage);
    }

    static BOOST_FORCEINLINE void notify_all(storage_type volatile& storage) BOOST_NOEXCEPT
    {
        ensure_wait_functions_initialized();

        if (BOOST_LIKELY(atomics::detail::wake_by_address_all != NULL))
            atomics::detail::wake_by_address_all(const_cast< storage_type* >(&storage));
        else
            base_type::notify_all(storage);
    }
};

#endif // defined(BOOST_ATOMIC_DETAIL_WINDOWS_HAS_WAIT_ON_ADDRESS)

template< typename Base >
struct wait_operations< Base, 1u, true, false > :
    public wait_operations_windows< Base, 1u >
{
};

template< typename Base >
struct wait_operations< Base, 2u, true, false > :
    public wait_operations_windows< Base, 2u >
{
};

template< typename Base >
struct wait_operations< Base, 4u, true, false > :
    public wait_operations_windows< Base, 4u >
{
};

template< typename Base >
struct wait_operations< Base, 8u, true, false > :
    public wait_operations_windows< Base, 8u >
{
};

} // namespace detail
} // namespace atomics
} // namespace boost

#include <boost/atomic/detail/footer.hpp>

#endif // BOOST_ATOMIC_DETAIL_WAIT_OPS_WINDOWS_HPP_INCLUDED_
