/*
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * Copyright (c) 2020 Andrey Semashev
 */
/*!
 * \file   wait_ops_windows.cpp
 *
 * This file contains implementation of the waiting/notifying atomic operations on Windows.
 *
 * https://docs.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitonaddress
 * https://docs.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-wakebyaddresssingle
 * https://docs.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-wakebyaddressall
 */

// Include boost/winapi/config.hpp first to make sure target Windows version is selected by Boost.WinAPI
#include <boost/winapi/config.hpp>

#include <boost/winapi/basic_types.hpp>
#include <boost/winapi/wait_on_address.hpp>

#include <boost/atomic/detail/config.hpp>
#include <boost/atomic/detail/link.hpp>
#include <boost/atomic/detail/once_flag.hpp>
#include <boost/atomic/detail/wait_ops_windows.hpp>

#if BOOST_USE_WINAPI_VERSION < BOOST_WINAPI_VERSION_WIN8
#include <boost/static_assert.hpp>
#include <boost/memory_order.hpp>
#include <boost/winapi/thread.hpp>
#include <boost/winapi/get_proc_address.hpp>
#include <boost/winapi/dll.hpp>

#include <boost/atomic/detail/core_operations.hpp>
#endif // BOOST_USE_WINAPI_VERSION < BOOST_WINAPI_VERSION_WIN8

#include <boost/atomic/detail/header.hpp>

namespace boost {
namespace atomics {
namespace detail {

#if BOOST_USE_WINAPI_VERSION < BOOST_WINAPI_VERSION_WIN8

BOOST_ATOMIC_DECL wait_on_address_t* wait_on_address = NULL;
BOOST_ATOMIC_DECL wake_by_address_t* wake_by_address_single = NULL;
BOOST_ATOMIC_DECL wake_by_address_t* wake_by_address_all = NULL;

BOOST_ATOMIC_DECL once_flag wait_functions_once_flag = { 2u };

BOOST_ATOMIC_DECL void initialize_wait_functions() BOOST_NOEXCEPT
{
    BOOST_STATIC_ASSERT_MSG(once_flag_operations::is_always_lock_free, "Boost.Atomic unsupported target platform: native atomic operations not implemented for bytes");

    once_flag_operations::storage_type old_val = once_flag_operations::load(wait_functions_once_flag.m_flag, boost::memory_order_acquire);
    while (true)
    {
        if (old_val == 2u)
        {
            if (BOOST_UNLIKELY(!once_flag_operations::compare_exchange_strong(wait_functions_once_flag.m_flag, old_val, 1u, boost::memory_order_relaxed, boost::memory_order_relaxed)))
                continue;

            boost::winapi::HMODULE_ kernel_base = boost::winapi::get_module_handle(L"api-ms-win-core-synch-l1-2-0.dll");
            if (BOOST_LIKELY(kernel_base != NULL))
            {
                wait_on_address_t* woa = (wait_on_address_t*)boost::winapi::get_proc_address(kernel_base, "WaitOnAddress");
                if (BOOST_LIKELY(woa != NULL))
                {
                    wake_by_address_t* wbas = (wake_by_address_t*)boost::winapi::get_proc_address(kernel_base, "WakeByAddressSingle");
                    wake_by_address_t* wbaa = (wake_by_address_t*)boost::winapi::get_proc_address(kernel_base, "WakeByAddressAll");

                    if (BOOST_LIKELY(wbas != NULL && wbaa != NULL))
                    {
                        wait_on_address = woa;
                        wake_by_address_single = wbas;
                        wake_by_address_all = wbaa;
                    }
                }
            }

            once_flag_operations::store(wait_functions_once_flag.m_flag, 0u, boost::memory_order_release);
            break;
        }
        else if (old_val == 1u)
        {
            boost::winapi::SwitchToThread();
            old_val = once_flag_operations::load(wait_functions_once_flag.m_flag, boost::memory_order_acquire);
        }
        else
        {
            break;
        }
    }
}

#else // BOOST_USE_WINAPI_VERSION < BOOST_WINAPI_VERSION_WIN8

BOOST_ATOMIC_DECL wait_on_address_t* wait_on_address = &boost::winapi::WaitOnAddress;
BOOST_ATOMIC_DECL wake_by_address_t* wake_by_address_single = &boost::winapi::WakeByAddressSingle;
BOOST_ATOMIC_DECL wake_by_address_t* wake_by_address_all = &boost::winapi::WakeByAddressAll;

BOOST_ATOMIC_DECL once_flag wait_functions_once_flag = { 0u };

BOOST_ATOMIC_DECL void initialize_wait_functions() BOOST_NOEXCEPT
{
}

#endif // BOOST_USE_WINAPI_VERSION < BOOST_WINAPI_VERSION_WIN8

} // namespace detail
} // namespace atomics
} // namespace boost

#include <boost/atomic/detail/footer.hpp>
