/*
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * Copyright (c) 2011 Helge Bahmann
 * Copyright (c) 2013 Tim Blechmann
 * Copyright (c) 2014-2020 Andrey Semashev
 */
/*!
 * \file   atomic/detail/atomic_impl.hpp
 *
 * This header contains implementation of \c atomic template.
 */

#ifndef BOOST_ATOMIC_DETAIL_ATOMIC_IMPL_HPP_INCLUDED_
#define BOOST_ATOMIC_DETAIL_ATOMIC_IMPL_HPP_INCLUDED_

#include <cstddef>
#include <boost/assert.hpp>
#include <boost/memory_order.hpp>
#include <boost/atomic/detail/config.hpp>
#include <boost/atomic/detail/intptr.hpp>
#include <boost/atomic/detail/storage_traits.hpp>
#include <boost/atomic/detail/bitwise_cast.hpp>
#include <boost/atomic/detail/integral_conversions.hpp>
#include <boost/atomic/detail/core_operations.hpp>
#include <boost/atomic/detail/wait_operations.hpp>
#include <boost/atomic/detail/extra_operations.hpp>
#include <boost/atomic/detail/memory_order_utils.hpp>
#include <boost/atomic/detail/aligned_variable.hpp>
#include <boost/atomic/detail/type_traits/is_signed.hpp>
#include <boost/atomic/detail/type_traits/is_nothrow_default_constructible.hpp>
#include <boost/atomic/detail/type_traits/is_trivially_default_constructible.hpp>
#include <boost/atomic/detail/type_traits/alignment_of.hpp>
#include <boost/atomic/detail/type_traits/conditional.hpp>
#include <boost/atomic/detail/type_traits/integral_constant.hpp>
#if !defined(BOOST_ATOMIC_NO_FLOATING_POINT)
#include <boost/atomic/detail/bitwise_fp_cast.hpp>
#include <boost/atomic/detail/fp_operations.hpp>
#include <boost/atomic/detail/extra_fp_operations.hpp>
#endif
#include <boost/atomic/detail/header.hpp>

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

#if !defined(BOOST_ATOMIC_DETAIL_NO_CXX11_CONSTEXPR_UNION_INIT) && !defined(BOOST_ATOMIC_DETAIL_NO_CXX11_CONSTEXPR_BITWISE_CAST)
#define BOOST_ATOMIC_DETAIL_CONSTEXPR_ATOMIC_CTOR BOOST_CONSTEXPR
#else
#define BOOST_ATOMIC_DETAIL_CONSTEXPR_ATOMIC_CTOR
#endif

/*
 * IMPLEMENTATION NOTE: All interface functions MUST be declared with BOOST_FORCEINLINE,
 *                      see comment for convert_memory_order_to_gcc in gcc_atomic_memory_order_utils.hpp.
 */

namespace boost {
namespace atomics {
namespace detail {

template< typename T, bool Signed, bool Interprocess >
class base_atomic_common
{
public:
    typedef T value_type;

protected:
    typedef atomics::detail::core_operations< storage_size_of< value_type >::value, Signed, Interprocess > core_operations;
    typedef atomics::detail::wait_operations< core_operations > wait_operations;
    typedef typename atomics::detail::conditional< sizeof(value_type) <= sizeof(void*), value_type, value_type const& >::type value_arg_type;
    typedef typename core_operations::storage_type storage_type;

protected:
    static BOOST_CONSTEXPR_OR_CONST std::size_t storage_alignment = atomics::detail::alignment_of< value_type >::value <= core_operations::storage_alignment ? core_operations::storage_alignment : atomics::detail::alignment_of< value_type >::value;

public:
    static BOOST_CONSTEXPR_OR_CONST bool is_always_lock_free = core_operations::is_always_lock_free;
    static BOOST_CONSTEXPR_OR_CONST bool always_has_native_wait_notify = wait_operations::always_has_native_wait_notify;

protected:
    BOOST_ATOMIC_DETAIL_ALIGNED_VAR_TPL(storage_alignment, storage_type, m_storage);

public:
    BOOST_FORCEINLINE BOOST_ATOMIC_DETAIL_CONSTEXPR_UNION_INIT base_atomic_common() BOOST_NOEXCEPT : m_storage()
    {
    }

    BOOST_FORCEINLINE BOOST_ATOMIC_DETAIL_CONSTEXPR_UNION_INIT explicit base_atomic_common(storage_type v) BOOST_NOEXCEPT : m_storage(v)
    {
    }

    BOOST_FORCEINLINE value_type& value() BOOST_NOEXCEPT { return *reinterpret_cast< value_type* >(&m_storage); }
    BOOST_FORCEINLINE value_type volatile& value() volatile BOOST_NOEXCEPT { return *reinterpret_cast< volatile value_type* >(&m_storage); }
    BOOST_FORCEINLINE value_type const& value() const BOOST_NOEXCEPT { return *reinterpret_cast< const value_type* >(&m_storage); }
    BOOST_FORCEINLINE value_type const volatile& value() const volatile BOOST_NOEXCEPT { return *reinterpret_cast< const volatile value_type* >(&m_storage); }

protected:
    BOOST_FORCEINLINE storage_type& storage() BOOST_NOEXCEPT { return m_storage; }
    BOOST_FORCEINLINE storage_type volatile& storage() volatile BOOST_NOEXCEPT { return m_storage; }
    BOOST_FORCEINLINE storage_type const& storage() const BOOST_NOEXCEPT { return m_storage; }
    BOOST_FORCEINLINE storage_type const volatile& storage() const volatile BOOST_NOEXCEPT { return m_storage; }

public:
    BOOST_FORCEINLINE bool is_lock_free() const volatile BOOST_NOEXCEPT
    {
        // C++17 requires all instances of atomic<> return a value consistent with is_always_lock_free here.
        // Boost.Atomic also enforces the required alignment of the atomic storage, so we can always return is_always_lock_free.
        return is_always_lock_free;
    }

    BOOST_FORCEINLINE bool has_native_wait_notify() const volatile BOOST_NOEXCEPT
    {
        return wait_operations::has_native_wait_notify(this->storage());
    }

    BOOST_FORCEINLINE void notify_one() volatile BOOST_NOEXCEPT
    {
        wait_operations::notify_one(this->storage());
    }

    BOOST_FORCEINLINE void notify_all() volatile BOOST_NOEXCEPT
    {
        wait_operations::notify_all(this->storage());
    }
};

#if defined(BOOST_NO_CXX17_INLINE_VARIABLES)
template< typename T, bool Signed, bool Interprocess >
BOOST_CONSTEXPR_OR_CONST bool base_atomic_common< T, Signed, Interprocess >::is_always_lock_free;
template< typename T, bool Signed, bool Interprocess >
BOOST_CONSTEXPR_OR_CONST bool base_atomic_common< T, Signed, Interprocess >::always_has_native_wait_notify;
#endif


template< typename T, bool Interprocess, bool IsTriviallyDefaultConstructible = atomics::detail::is_trivially_default_constructible< T >::value >
class base_atomic_generic;

template< typename T, bool Interprocess >
class base_atomic_generic< T, Interprocess, true > :
    public base_atomic_common< T, false, Interprocess >
{
private:
    typedef base_atomic_common< T, false, Interprocess > base_type;

protected:
    typedef typename base_type::storage_type storage_type;
    typedef typename base_type::value_arg_type value_arg_type;

public:
    BOOST_DEFAULTED_FUNCTION(base_atomic_generic() BOOST_ATOMIC_DETAIL_DEF_NOEXCEPT_DECL, BOOST_ATOMIC_DETAIL_DEF_NOEXCEPT_IMPL {})
    BOOST_FORCEINLINE BOOST_ATOMIC_DETAIL_CONSTEXPR_ATOMIC_CTOR explicit base_atomic_generic(value_arg_type v) BOOST_NOEXCEPT :
        base_type(atomics::detail::bitwise_cast< storage_type >(v))
    {
    }
};

template< typename T, bool Interprocess >
class base_atomic_generic< T, Interprocess, false > :
    public base_atomic_common< T, false, Interprocess >
{
private:
    typedef base_atomic_common< T, false, Interprocess > base_type;

public:
    typedef typename base_type::value_type value_type;

protected:
    typedef typename base_type::storage_type storage_type;
    typedef typename base_type::value_arg_type value_arg_type;

public:
    BOOST_FORCEINLINE BOOST_ATOMIC_DETAIL_CONSTEXPR_ATOMIC_CTOR explicit base_atomic_generic(value_arg_type v = value_type()) BOOST_NOEXCEPT :
        base_type(atomics::detail::bitwise_cast< storage_type >(v))
    {
    }
};


template< typename T, typename Kind, bool Interprocess >
class base_atomic;

//! General template. Implementation for user-defined types, such as structs, and pointers to non-object types
template< typename T, bool Interprocess >
class base_atomic< T, void, Interprocess > :
    public base_atomic_generic< T, Interprocess >
{
private:
    typedef base_atomic_generic< T, Interprocess > base_type;

public:
    typedef typename base_type::value_type value_type;

protected:
    typedef typename base_type::core_operations core_operations;
    typedef typename base_type::wait_operations wait_operations;
    typedef typename base_type::storage_type storage_type;
    typedef typename base_type::value_arg_type value_arg_type;

private:
#if !defined(BOOST_ATOMIC_DETAIL_STORAGE_TYPE_MAY_ALIAS) || !defined(BOOST_ATOMIC_NO_CLEAR_PADDING)
    typedef atomics::detail::true_type cxchg_use_bitwise_cast;
#else
    typedef atomics::detail::integral_constant< bool, sizeof(value_type) != sizeof(storage_type) || atomics::detail::alignment_of< value_type >::value <= core_operations::storage_alignment > cxchg_use_bitwise_cast;
#endif

public:
    BOOST_FORCEINLINE BOOST_ATOMIC_DETAIL_CONSTEXPR_ATOMIC_CTOR base_atomic() BOOST_NOEXCEPT_IF(atomics::detail::is_nothrow_default_constructible< value_type >::value) : base_type()
    {
    }

    BOOST_FORCEINLINE BOOST_ATOMIC_DETAIL_CONSTEXPR_ATOMIC_CTOR explicit base_atomic(value_arg_type v) BOOST_NOEXCEPT : base_type(v)
    {
    }

    BOOST_FORCEINLINE void store(value_arg_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        BOOST_ASSERT(order != memory_order_consume);
        BOOST_ASSERT(order != memory_order_acquire);
        BOOST_ASSERT(order != memory_order_acq_rel);

        core_operations::store(this->storage(), atomics::detail::bitwise_cast< storage_type >(v), order);
    }

    BOOST_FORCEINLINE value_type load(memory_order order = memory_order_seq_cst) const volatile BOOST_NOEXCEPT
    {
        BOOST_ASSERT(order != memory_order_release);
        BOOST_ASSERT(order != memory_order_acq_rel);

        return atomics::detail::bitwise_cast< value_type >(core_operations::load(this->storage(), order));
    }

    BOOST_FORCEINLINE value_type exchange(value_arg_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return atomics::detail::bitwise_cast< value_type >(core_operations::exchange(this->storage(), atomics::detail::bitwise_cast< storage_type >(v), order));
    }

    BOOST_FORCEINLINE bool compare_exchange_strong(value_type& expected, value_arg_type desired, memory_order success_order, memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        BOOST_ASSERT(failure_order != memory_order_release);
        BOOST_ASSERT(failure_order != memory_order_acq_rel);
        BOOST_ASSERT(cas_failure_order_must_not_be_stronger_than_success_order(success_order, failure_order));

        return compare_exchange_strong_impl(expected, desired, success_order, failure_order, cxchg_use_bitwise_cast());
    }

    BOOST_FORCEINLINE bool compare_exchange_strong(value_type& expected, value_arg_type desired, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return compare_exchange_strong(expected, desired, order, atomics::detail::deduce_failure_order(order));
    }

    BOOST_FORCEINLINE bool compare_exchange_weak(value_type& expected, value_arg_type desired, memory_order success_order, memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        BOOST_ASSERT(failure_order != memory_order_release);
        BOOST_ASSERT(failure_order != memory_order_acq_rel);
        BOOST_ASSERT(cas_failure_order_must_not_be_stronger_than_success_order(success_order, failure_order));

        return compare_exchange_weak_impl(expected, desired, success_order, failure_order, cxchg_use_bitwise_cast());
    }

    BOOST_FORCEINLINE bool compare_exchange_weak(value_type& expected, value_arg_type desired, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return compare_exchange_weak(expected, desired, order, atomics::detail::deduce_failure_order(order));
    }

    BOOST_FORCEINLINE value_type wait(value_arg_type old_val, memory_order order = memory_order_seq_cst) const volatile BOOST_NOEXCEPT
    {
        BOOST_ASSERT(order != memory_order_release);
        BOOST_ASSERT(order != memory_order_acq_rel);

        return atomics::detail::bitwise_cast< value_type >(wait_operations::wait(this->storage(), atomics::detail::bitwise_cast< storage_type >(old_val), order));
    }

    BOOST_DELETED_FUNCTION(base_atomic(base_atomic const&))
    BOOST_DELETED_FUNCTION(base_atomic& operator=(base_atomic const&))

private:
    BOOST_FORCEINLINE bool compare_exchange_strong_impl(value_type& expected, value_arg_type desired, memory_order success_order, memory_order failure_order, atomics::detail::false_type) volatile BOOST_NOEXCEPT
    {
        return core_operations::compare_exchange_strong(this->storage(), reinterpret_cast< storage_type& >(expected), atomics::detail::bitwise_cast< storage_type >(desired), success_order, failure_order);
    }

    BOOST_FORCEINLINE bool compare_exchange_strong_impl(value_type& expected, value_arg_type desired, memory_order success_order, memory_order failure_order, atomics::detail::true_type) volatile BOOST_NOEXCEPT
    {
        storage_type old_value = atomics::detail::bitwise_cast< storage_type >(expected);
        const bool res = core_operations::compare_exchange_strong(this->storage(), old_value, atomics::detail::bitwise_cast< storage_type >(desired), success_order, failure_order);
        expected = atomics::detail::bitwise_cast< value_type >(old_value);
        return res;
    }

    BOOST_FORCEINLINE bool compare_exchange_weak_impl(value_type& expected, value_arg_type desired, memory_order success_order, memory_order failure_order, atomics::detail::false_type) volatile BOOST_NOEXCEPT
    {
        return core_operations::compare_exchange_weak(this->storage(), reinterpret_cast< storage_type& >(expected), atomics::detail::bitwise_cast< storage_type >(desired), success_order, failure_order);
    }

    BOOST_FORCEINLINE bool compare_exchange_weak_impl(value_type& expected, value_arg_type desired, memory_order success_order, memory_order failure_order, atomics::detail::true_type) volatile BOOST_NOEXCEPT
    {
        storage_type old_value = atomics::detail::bitwise_cast< storage_type >(expected);
        const bool res = core_operations::compare_exchange_weak(this->storage(), old_value, atomics::detail::bitwise_cast< storage_type >(desired), success_order, failure_order);
        expected = atomics::detail::bitwise_cast< value_type >(old_value);
        return res;
    }
};


//! Implementation for enums
template< typename T, bool Interprocess >
class base_atomic< T, const int, Interprocess > :
    public base_atomic_common< T, false, Interprocess >
{
private:
    typedef base_atomic_common< T, false, Interprocess > base_type;

public:
    typedef typename base_type::value_type value_type;

protected:
    typedef typename base_type::core_operations core_operations;
    typedef typename base_type::wait_operations wait_operations;
    typedef typename base_type::storage_type storage_type;
    typedef typename base_type::value_arg_type value_arg_type;

private:
#if !defined(BOOST_ATOMIC_DETAIL_STORAGE_TYPE_MAY_ALIAS) || !defined(BOOST_ATOMIC_NO_CLEAR_PADDING)
    typedef atomics::detail::true_type cxchg_use_bitwise_cast;
#else
    typedef atomics::detail::integral_constant< bool, sizeof(value_type) != sizeof(storage_type) || atomics::detail::alignment_of< value_type >::value <= core_operations::storage_alignment > cxchg_use_bitwise_cast;
#endif

public:
    BOOST_DEFAULTED_FUNCTION(base_atomic() BOOST_ATOMIC_DETAIL_DEF_NOEXCEPT_DECL, BOOST_ATOMIC_DETAIL_DEF_NOEXCEPT_IMPL {})
    BOOST_FORCEINLINE BOOST_ATOMIC_DETAIL_CONSTEXPR_UNION_INIT explicit base_atomic(value_arg_type v) BOOST_NOEXCEPT : base_type(static_cast< storage_type >(v))
    {
    }

    BOOST_FORCEINLINE void store(value_arg_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        BOOST_ASSERT(order != memory_order_consume);
        BOOST_ASSERT(order != memory_order_acquire);
        BOOST_ASSERT(order != memory_order_acq_rel);

        core_operations::store(this->storage(), static_cast< storage_type >(v), order);
    }

    BOOST_FORCEINLINE value_type load(memory_order order = memory_order_seq_cst) const volatile BOOST_NOEXCEPT
    {
        BOOST_ASSERT(order != memory_order_release);
        BOOST_ASSERT(order != memory_order_acq_rel);

        return atomics::detail::bitwise_cast< value_type >(core_operations::load(this->storage(), order));
    }

    BOOST_FORCEINLINE value_type exchange(value_arg_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return atomics::detail::bitwise_cast< value_type >(core_operations::exchange(this->storage(), static_cast< storage_type >(v), order));
    }

    BOOST_FORCEINLINE bool compare_exchange_strong(value_type& expected, value_arg_type desired, memory_order success_order, memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        BOOST_ASSERT(failure_order != memory_order_release);
        BOOST_ASSERT(failure_order != memory_order_acq_rel);
        BOOST_ASSERT(cas_failure_order_must_not_be_stronger_than_success_order(success_order, failure_order));

        return compare_exchange_strong_impl(expected, desired, success_order, failure_order, cxchg_use_bitwise_cast());
    }

    BOOST_FORCEINLINE bool compare_exchange_strong(value_type& expected, value_arg_type desired, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return compare_exchange_strong(expected, desired, order, atomics::detail::deduce_failure_order(order));
    }

    BOOST_FORCEINLINE bool compare_exchange_weak(value_type& expected, value_arg_type desired, memory_order success_order, memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        BOOST_ASSERT(failure_order != memory_order_release);
        BOOST_ASSERT(failure_order != memory_order_acq_rel);
        BOOST_ASSERT(cas_failure_order_must_not_be_stronger_than_success_order(success_order, failure_order));

        return compare_exchange_weak_impl(expected, desired, success_order, failure_order, cxchg_use_bitwise_cast());
    }

    BOOST_FORCEINLINE bool compare_exchange_weak(value_type& expected, value_arg_type desired, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return compare_exchange_weak(expected, desired, order, atomics::detail::deduce_failure_order(order));
    }

    BOOST_FORCEINLINE value_type wait(value_arg_type old_val, memory_order order = memory_order_seq_cst) const volatile BOOST_NOEXCEPT
    {
        BOOST_ASSERT(order != memory_order_release);
        BOOST_ASSERT(order != memory_order_acq_rel);

        return atomics::detail::bitwise_cast< value_type >(wait_operations::wait(this->storage(), static_cast< storage_type >(old_val), order));
    }

    BOOST_DELETED_FUNCTION(base_atomic(base_atomic const&))
    BOOST_DELETED_FUNCTION(base_atomic& operator=(base_atomic const&))

private:
    BOOST_FORCEINLINE bool compare_exchange_strong_impl(value_type& expected, value_arg_type desired, memory_order success_order, memory_order failure_order, atomics::detail::false_type) volatile BOOST_NOEXCEPT
    {
        return core_operations::compare_exchange_strong(this->storage(), reinterpret_cast< storage_type& >(expected), static_cast< storage_type >(desired), success_order, failure_order);
    }

    BOOST_FORCEINLINE bool compare_exchange_strong_impl(value_type& expected, value_arg_type desired, memory_order success_order, memory_order failure_order, atomics::detail::true_type) volatile BOOST_NOEXCEPT
    {
        storage_type old_value = static_cast< storage_type >(expected);
        const bool res = core_operations::compare_exchange_strong(this->storage(), old_value, static_cast< storage_type >(desired), success_order, failure_order);
        expected = atomics::detail::bitwise_cast< value_type >(old_value);
        return res;
    }

    BOOST_FORCEINLINE bool compare_exchange_weak_impl(value_type& expected, value_arg_type desired, memory_order success_order, memory_order failure_order, atomics::detail::false_type) volatile BOOST_NOEXCEPT
    {
        return core_operations::compare_exchange_weak(this->storage(), reinterpret_cast< storage_type& >(expected), static_cast< storage_type >(desired), success_order, failure_order);
    }

    BOOST_FORCEINLINE bool compare_exchange_weak_impl(value_type& expected, value_arg_type desired, memory_order success_order, memory_order failure_order, atomics::detail::true_type) volatile BOOST_NOEXCEPT
    {
        storage_type old_value = static_cast< storage_type >(expected);
        const bool res = core_operations::compare_exchange_weak(this->storage(), old_value, static_cast< storage_type >(desired), success_order, failure_order);
        expected = atomics::detail::bitwise_cast< value_type >(old_value);
        return res;
    }
};


//! Implementation for integers
template< typename T, bool Interprocess >
class base_atomic< T, int, Interprocess > :
    public base_atomic_common< T, atomics::detail::is_signed< T >::value, Interprocess >
{
private:
    typedef base_atomic_common< T, atomics::detail::is_signed< T >::value, Interprocess > base_type;

public:
    typedef typename base_type::value_type value_type;
    typedef value_type difference_type;

protected:
    typedef typename base_type::core_operations core_operations;
    typedef typename base_type::wait_operations wait_operations;
    typedef atomics::detail::extra_operations< core_operations > extra_operations;
    typedef typename base_type::storage_type storage_type;
    typedef value_type value_arg_type;

private:
#if !defined(BOOST_ATOMIC_DETAIL_STORAGE_TYPE_MAY_ALIAS) || !defined(BOOST_ATOMIC_NO_CLEAR_PADDING)
    typedef atomics::detail::true_type cxchg_use_bitwise_cast;
#else
    typedef atomics::detail::integral_constant< bool, sizeof(value_type) != sizeof(storage_type) || atomics::detail::alignment_of< value_type >::value <= core_operations::storage_alignment > cxchg_use_bitwise_cast;
#endif

public:
    BOOST_DEFAULTED_FUNCTION(base_atomic() BOOST_ATOMIC_DETAIL_DEF_NOEXCEPT_DECL, BOOST_ATOMIC_DETAIL_DEF_NOEXCEPT_IMPL {})
    BOOST_FORCEINLINE BOOST_ATOMIC_DETAIL_CONSTEXPR_UNION_INIT explicit base_atomic(value_arg_type v) BOOST_NOEXCEPT : base_type(static_cast< storage_type >(v)) {}

    // Standard methods
    BOOST_FORCEINLINE void store(value_arg_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        BOOST_ASSERT(order != memory_order_consume);
        BOOST_ASSERT(order != memory_order_acquire);
        BOOST_ASSERT(order != memory_order_acq_rel);

        core_operations::store(this->storage(), static_cast< storage_type >(v), order);
    }

    BOOST_FORCEINLINE value_type load(memory_order order = memory_order_seq_cst) const volatile BOOST_NOEXCEPT
    {
        BOOST_ASSERT(order != memory_order_release);
        BOOST_ASSERT(order != memory_order_acq_rel);

        return atomics::detail::integral_truncate< value_type >(core_operations::load(this->storage(), order));
    }

    BOOST_FORCEINLINE value_type fetch_add(difference_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return atomics::detail::integral_truncate< value_type >(core_operations::fetch_add(this->storage(), static_cast< storage_type >(v), order));
    }

    BOOST_FORCEINLINE value_type fetch_sub(difference_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return atomics::detail::integral_truncate< value_type >(core_operations::fetch_sub(this->storage(), static_cast< storage_type >(v), order));
    }

    BOOST_FORCEINLINE value_type exchange(value_arg_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return atomics::detail::integral_truncate< value_type >(core_operations::exchange(this->storage(), static_cast< storage_type >(v), order));
    }

    BOOST_FORCEINLINE bool compare_exchange_strong(value_type& expected, value_arg_type desired, memory_order success_order, memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        BOOST_ASSERT(failure_order != memory_order_release);
        BOOST_ASSERT(failure_order != memory_order_acq_rel);
        BOOST_ASSERT(cas_failure_order_must_not_be_stronger_than_success_order(success_order, failure_order));

        return compare_exchange_strong_impl(expected, desired, success_order, failure_order, cxchg_use_bitwise_cast());
    }

    BOOST_FORCEINLINE bool compare_exchange_strong(value_type& expected, value_arg_type desired, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return compare_exchange_strong(expected, desired, order, atomics::detail::deduce_failure_order(order));
    }

    BOOST_FORCEINLINE bool compare_exchange_weak(value_type& expected, value_arg_type desired, memory_order success_order, memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        BOOST_ASSERT(failure_order != memory_order_release);
        BOOST_ASSERT(failure_order != memory_order_acq_rel);
        BOOST_ASSERT(cas_failure_order_must_not_be_stronger_than_success_order(success_order, failure_order));

        return compare_exchange_weak_impl(expected, desired, success_order, failure_order, cxchg_use_bitwise_cast());
    }

    BOOST_FORCEINLINE bool compare_exchange_weak(value_type& expected, value_arg_type desired, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return compare_exchange_weak(expected, desired, order, atomics::detail::deduce_failure_order(order));
    }

    BOOST_FORCEINLINE value_type fetch_and(value_arg_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return atomics::detail::integral_truncate< value_type >(core_operations::fetch_and(this->storage(), static_cast< storage_type >(v), order));
    }

    BOOST_FORCEINLINE value_type fetch_or(value_arg_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return atomics::detail::integral_truncate< value_type >(core_operations::fetch_or(this->storage(), static_cast< storage_type >(v), order));
    }

    BOOST_FORCEINLINE value_type fetch_xor(value_arg_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return atomics::detail::integral_truncate< value_type >(core_operations::fetch_xor(this->storage(), static_cast< storage_type >(v), order));
    }

    // Boost.Atomic extensions
    BOOST_FORCEINLINE value_type fetch_negate(memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return atomics::detail::integral_truncate< value_type >(extra_operations::fetch_negate(this->storage(), order));
    }

    BOOST_FORCEINLINE value_type fetch_complement(memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return atomics::detail::integral_truncate< value_type >(extra_operations::fetch_complement(this->storage(), order));
    }

    BOOST_FORCEINLINE value_type add(difference_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return atomics::detail::integral_truncate< value_type >(extra_operations::add(this->storage(), static_cast< storage_type >(v), order));
    }

    BOOST_FORCEINLINE value_type sub(difference_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return atomics::detail::integral_truncate< value_type >(extra_operations::sub(this->storage(), static_cast< storage_type >(v), order));
    }

    BOOST_FORCEINLINE value_type negate(memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return atomics::detail::integral_truncate< value_type >(extra_operations::negate(this->storage(), order));
    }

    BOOST_FORCEINLINE value_type bitwise_and(value_arg_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return atomics::detail::integral_truncate< value_type >(extra_operations::bitwise_and(this->storage(), static_cast< storage_type >(v), order));
    }

    BOOST_FORCEINLINE value_type bitwise_or(value_arg_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return atomics::detail::integral_truncate< value_type >(extra_operations::bitwise_or(this->storage(), static_cast< storage_type >(v), order));
    }

    BOOST_FORCEINLINE value_type bitwise_xor(value_arg_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return atomics::detail::integral_truncate< value_type >(extra_operations::bitwise_xor(this->storage(), static_cast< storage_type >(v), order));
    }

    BOOST_FORCEINLINE value_type bitwise_complement(memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return atomics::detail::integral_truncate< value_type >(extra_operations::bitwise_complement(this->storage(), order));
    }

    BOOST_FORCEINLINE void opaque_add(difference_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        extra_operations::opaque_add(this->storage(), static_cast< storage_type >(v), order);
    }

    BOOST_FORCEINLINE void opaque_sub(difference_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        extra_operations::opaque_sub(this->storage(), static_cast< storage_type >(v), order);
    }

    BOOST_FORCEINLINE void opaque_negate(memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        extra_operations::opaque_negate(this->storage(), order);
    }

    BOOST_FORCEINLINE void opaque_and(value_arg_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        extra_operations::opaque_and(this->storage(), static_cast< storage_type >(v), order);
    }

    BOOST_FORCEINLINE void opaque_or(value_arg_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        extra_operations::opaque_or(this->storage(), static_cast< storage_type >(v), order);
    }

    BOOST_FORCEINLINE void opaque_xor(value_arg_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        extra_operations::opaque_xor(this->storage(), static_cast< storage_type >(v), order);
    }

    BOOST_FORCEINLINE void opaque_complement(memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        extra_operations::opaque_complement(this->storage(), order);
    }

    BOOST_FORCEINLINE bool add_and_test(difference_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return extra_operations::add_and_test(this->storage(), static_cast< storage_type >(v), order);
    }

    BOOST_FORCEINLINE bool sub_and_test(difference_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return extra_operations::sub_and_test(this->storage(), static_cast< storage_type >(v), order);
    }

    BOOST_FORCEINLINE bool negate_and_test(memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return extra_operations::negate_and_test(this->storage(), order);
    }

    BOOST_FORCEINLINE bool and_and_test(value_arg_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return extra_operations::and_and_test(this->storage(), static_cast< storage_type >(v), order);
    }

    BOOST_FORCEINLINE bool or_and_test(value_arg_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return extra_operations::or_and_test(this->storage(), static_cast< storage_type >(v), order);
    }

    BOOST_FORCEINLINE bool xor_and_test(value_arg_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return extra_operations::xor_and_test(this->storage(), static_cast< storage_type >(v), order);
    }

    BOOST_FORCEINLINE bool complement_and_test(memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return extra_operations::complement_and_test(this->storage(), order);
    }

    BOOST_FORCEINLINE bool bit_test_and_set(unsigned int bit_number, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        BOOST_ASSERT(bit_number < sizeof(value_type) * 8u);
        return extra_operations::bit_test_and_set(this->storage(), bit_number, order);
    }

    BOOST_FORCEINLINE bool bit_test_and_reset(unsigned int bit_number, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        BOOST_ASSERT(bit_number < sizeof(value_type) * 8u);
        return extra_operations::bit_test_and_reset(this->storage(), bit_number, order);
    }

    BOOST_FORCEINLINE bool bit_test_and_complement(unsigned int bit_number, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        BOOST_ASSERT(bit_number < sizeof(value_type) * 8u);
        return extra_operations::bit_test_and_complement(this->storage(), bit_number, order);
    }

    // Operators
    BOOST_FORCEINLINE value_type operator++(int) volatile BOOST_NOEXCEPT
    {
        return fetch_add(1);
    }

    BOOST_FORCEINLINE value_type operator++() volatile BOOST_NOEXCEPT
    {
        return add(1);
    }

    BOOST_FORCEINLINE value_type operator--(int) volatile BOOST_NOEXCEPT
    {
        return fetch_sub(1);
    }

    BOOST_FORCEINLINE value_type operator--() volatile BOOST_NOEXCEPT
    {
        return sub(1);
    }

    BOOST_FORCEINLINE value_type operator+=(difference_type v) volatile BOOST_NOEXCEPT
    {
        return add(v);
    }

    BOOST_FORCEINLINE value_type operator-=(difference_type v) volatile BOOST_NOEXCEPT
    {
        return sub(v);
    }

    BOOST_FORCEINLINE value_type operator&=(value_type v) volatile BOOST_NOEXCEPT
    {
        return bitwise_and(v);
    }

    BOOST_FORCEINLINE value_type operator|=(value_type v) volatile BOOST_NOEXCEPT
    {
        return bitwise_or(v);
    }

    BOOST_FORCEINLINE value_type operator^=(value_type v) volatile BOOST_NOEXCEPT
    {
        return bitwise_xor(v);
    }

    BOOST_FORCEINLINE value_type wait(value_type old_val, memory_order order = memory_order_seq_cst) const volatile BOOST_NOEXCEPT
    {
        BOOST_ASSERT(order != memory_order_release);
        BOOST_ASSERT(order != memory_order_acq_rel);

        return atomics::detail::integral_truncate< value_type >(wait_operations::wait(this->storage(), static_cast< storage_type >(old_val), order));
    }

    BOOST_DELETED_FUNCTION(base_atomic(base_atomic const&))
    BOOST_DELETED_FUNCTION(base_atomic& operator=(base_atomic const&))

private:
    BOOST_FORCEINLINE bool compare_exchange_strong_impl(value_type& expected, value_arg_type desired, memory_order success_order, memory_order failure_order, atomics::detail::false_type) volatile BOOST_NOEXCEPT
    {
        return core_operations::compare_exchange_strong(this->storage(), reinterpret_cast< storage_type& >(expected), static_cast< storage_type >(desired), success_order, failure_order);
    }

    BOOST_FORCEINLINE bool compare_exchange_strong_impl(value_type& expected, value_arg_type desired, memory_order success_order, memory_order failure_order, atomics::detail::true_type) volatile BOOST_NOEXCEPT
    {
        storage_type old_value = static_cast< storage_type >(expected);
        const bool res = core_operations::compare_exchange_strong(this->storage(), old_value, static_cast< storage_type >(desired), success_order, failure_order);
        expected = atomics::detail::integral_truncate< value_type >(old_value);
        return res;
    }

    BOOST_FORCEINLINE bool compare_exchange_weak_impl(value_type& expected, value_arg_type desired, memory_order success_order, memory_order failure_order, atomics::detail::false_type) volatile BOOST_NOEXCEPT
    {
        return core_operations::compare_exchange_weak(this->storage(), reinterpret_cast< storage_type& >(expected), static_cast< storage_type >(desired), success_order, failure_order);
    }

    BOOST_FORCEINLINE bool compare_exchange_weak_impl(value_type& expected, value_arg_type desired, memory_order success_order, memory_order failure_order, atomics::detail::true_type) volatile BOOST_NOEXCEPT
    {
        storage_type old_value = static_cast< storage_type >(expected);
        const bool res = core_operations::compare_exchange_weak(this->storage(), old_value, static_cast< storage_type >(desired), success_order, failure_order);
        expected = atomics::detail::integral_truncate< value_type >(old_value);
        return res;
    }
};

//! Implementation for bool
template< bool Interprocess >
class base_atomic< bool, int, Interprocess > :
    public base_atomic_common< bool, false, Interprocess >
{
private:
    typedef base_atomic_common< bool, false, Interprocess > base_type;

public:
    typedef typename base_type::value_type value_type;

protected:
    typedef typename base_type::core_operations core_operations;
    typedef typename base_type::wait_operations wait_operations;
    typedef typename base_type::storage_type storage_type;
    typedef value_type value_arg_type;

private:
#if !defined(BOOST_ATOMIC_DETAIL_STORAGE_TYPE_MAY_ALIAS) || !defined(BOOST_ATOMIC_NO_CLEAR_PADDING)
    typedef atomics::detail::true_type cxchg_use_bitwise_cast;
#else
    typedef atomics::detail::integral_constant< bool, sizeof(value_type) != sizeof(storage_type) || atomics::detail::alignment_of< value_type >::value <= core_operations::storage_alignment > cxchg_use_bitwise_cast;
#endif

public:
    BOOST_DEFAULTED_FUNCTION(base_atomic() BOOST_ATOMIC_DETAIL_DEF_NOEXCEPT_DECL, BOOST_ATOMIC_DETAIL_DEF_NOEXCEPT_IMPL {})
    BOOST_FORCEINLINE BOOST_ATOMIC_DETAIL_CONSTEXPR_UNION_INIT explicit base_atomic(value_arg_type v) BOOST_NOEXCEPT : base_type(static_cast< storage_type >(v)) {}

    // Standard methods
    BOOST_FORCEINLINE void store(value_arg_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        BOOST_ASSERT(order != memory_order_consume);
        BOOST_ASSERT(order != memory_order_acquire);
        BOOST_ASSERT(order != memory_order_acq_rel);

        core_operations::store(this->storage(), static_cast< storage_type >(v), order);
    }

    BOOST_FORCEINLINE value_type load(memory_order order = memory_order_seq_cst) const volatile BOOST_NOEXCEPT
    {
        BOOST_ASSERT(order != memory_order_release);
        BOOST_ASSERT(order != memory_order_acq_rel);

        return !!core_operations::load(this->storage(), order);
    }

    BOOST_FORCEINLINE value_type exchange(value_arg_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return !!core_operations::exchange(this->storage(), static_cast< storage_type >(v), order);
    }

    BOOST_FORCEINLINE bool compare_exchange_strong(value_type& expected, value_arg_type desired, memory_order success_order, memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        BOOST_ASSERT(failure_order != memory_order_release);
        BOOST_ASSERT(failure_order != memory_order_acq_rel);
        BOOST_ASSERT(cas_failure_order_must_not_be_stronger_than_success_order(success_order, failure_order));

        return compare_exchange_strong_impl(expected, desired, success_order, failure_order, cxchg_use_bitwise_cast());
    }

    BOOST_FORCEINLINE bool compare_exchange_strong(value_type& expected, value_arg_type desired, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return compare_exchange_strong(expected, desired, order, atomics::detail::deduce_failure_order(order));
    }

    BOOST_FORCEINLINE bool compare_exchange_weak(value_type& expected, value_arg_type desired, memory_order success_order, memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        BOOST_ASSERT(failure_order != memory_order_release);
        BOOST_ASSERT(failure_order != memory_order_acq_rel);
        BOOST_ASSERT(cas_failure_order_must_not_be_stronger_than_success_order(success_order, failure_order));

        return compare_exchange_weak_impl(expected, desired, success_order, failure_order, cxchg_use_bitwise_cast());
    }

    BOOST_FORCEINLINE bool compare_exchange_weak(value_type& expected, value_arg_type desired, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return compare_exchange_weak(expected, desired, order, atomics::detail::deduce_failure_order(order));
    }

    BOOST_FORCEINLINE value_type wait(value_type old_val, memory_order order = memory_order_seq_cst) const volatile BOOST_NOEXCEPT
    {
        BOOST_ASSERT(order != memory_order_release);
        BOOST_ASSERT(order != memory_order_acq_rel);

        return !!wait_operations::wait(this->storage(), static_cast< storage_type >(old_val), order);
    }

    BOOST_DELETED_FUNCTION(base_atomic(base_atomic const&))
    BOOST_DELETED_FUNCTION(base_atomic& operator=(base_atomic const&))

private:
    BOOST_FORCEINLINE bool compare_exchange_strong_impl(value_type& expected, value_arg_type desired, memory_order success_order, memory_order failure_order, atomics::detail::false_type) volatile BOOST_NOEXCEPT
    {
        return core_operations::compare_exchange_strong(this->storage(), reinterpret_cast< storage_type& >(expected), static_cast< storage_type >(desired), success_order, failure_order);
    }

    BOOST_FORCEINLINE bool compare_exchange_strong_impl(value_type& expected, value_arg_type desired, memory_order success_order, memory_order failure_order, atomics::detail::true_type) volatile BOOST_NOEXCEPT
    {
        storage_type old_value = static_cast< storage_type >(expected);
        const bool res = core_operations::compare_exchange_strong(this->storage(), old_value, static_cast< storage_type >(desired), success_order, failure_order);
        expected = !!old_value;
        return res;
    }

    BOOST_FORCEINLINE bool compare_exchange_weak_impl(value_type& expected, value_arg_type desired, memory_order success_order, memory_order failure_order, atomics::detail::false_type) volatile BOOST_NOEXCEPT
    {
        return core_operations::compare_exchange_weak(this->storage(), reinterpret_cast< storage_type& >(expected), static_cast< storage_type >(desired), success_order, failure_order);
    }

    BOOST_FORCEINLINE bool compare_exchange_weak_impl(value_type& expected, value_arg_type desired, memory_order success_order, memory_order failure_order, atomics::detail::true_type) volatile BOOST_NOEXCEPT
    {
        storage_type old_value = static_cast< storage_type >(expected);
        const bool res = core_operations::compare_exchange_weak(this->storage(), old_value, static_cast< storage_type >(desired), success_order, failure_order);
        expected = !!old_value;
        return res;
    }
};


#if !defined(BOOST_ATOMIC_NO_FLOATING_POINT)

//! Implementation for floating point types
template< typename T, bool Interprocess >
class base_atomic< T, float, Interprocess > :
    public base_atomic_common< T, false, Interprocess >
{
private:
    typedef base_atomic_common< T, false, Interprocess > base_type;

public:
    typedef typename base_type::value_type value_type;
    typedef value_type difference_type;

protected:
    typedef typename base_type::core_operations core_operations;
    typedef typename base_type::wait_operations wait_operations;
    typedef atomics::detail::extra_operations< core_operations > extra_operations;
    typedef atomics::detail::fp_operations< extra_operations, value_type > fp_operations;
    typedef atomics::detail::extra_fp_operations< fp_operations > extra_fp_operations;
    typedef typename base_type::storage_type storage_type;
    typedef value_type value_arg_type;

private:
#if !defined(BOOST_ATOMIC_DETAIL_STORAGE_TYPE_MAY_ALIAS) || !defined(BOOST_ATOMIC_NO_CLEAR_PADDING)
    typedef atomics::detail::true_type cxchg_use_bitwise_cast;
#else
    typedef atomics::detail::integral_constant< bool,
        atomics::detail::value_size_of< value_type >::value != sizeof(storage_type) || atomics::detail::alignment_of< value_type >::value <= core_operations::storage_alignment
    > cxchg_use_bitwise_cast;
#endif

public:
    BOOST_DEFAULTED_FUNCTION(base_atomic() BOOST_ATOMIC_DETAIL_DEF_NOEXCEPT_DECL, BOOST_ATOMIC_DETAIL_DEF_NOEXCEPT_IMPL {})
    BOOST_FORCEINLINE BOOST_ATOMIC_DETAIL_CONSTEXPR_ATOMIC_CTOR explicit base_atomic(value_arg_type v) BOOST_NOEXCEPT :
        base_type(atomics::detail::bitwise_fp_cast< storage_type >(v))
    {
    }

    BOOST_FORCEINLINE void store(value_arg_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        BOOST_ASSERT(order != memory_order_consume);
        BOOST_ASSERT(order != memory_order_acquire);
        BOOST_ASSERT(order != memory_order_acq_rel);

        core_operations::store(this->storage(), atomics::detail::bitwise_fp_cast< storage_type >(v), order);
    }

    BOOST_FORCEINLINE value_type load(memory_order order = memory_order_seq_cst) const volatile BOOST_NOEXCEPT
    {
        BOOST_ASSERT(order != memory_order_release);
        BOOST_ASSERT(order != memory_order_acq_rel);

        return atomics::detail::bitwise_fp_cast< value_type >(core_operations::load(this->storage(), order));
    }

    BOOST_FORCEINLINE value_type fetch_add(difference_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return fp_operations::fetch_add(this->storage(), v, order);
    }

    BOOST_FORCEINLINE value_type fetch_sub(difference_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return fp_operations::fetch_sub(this->storage(), v, order);
    }

    BOOST_FORCEINLINE value_type exchange(value_arg_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return atomics::detail::bitwise_fp_cast< value_type >(core_operations::exchange(this->storage(), atomics::detail::bitwise_fp_cast< storage_type >(v), order));
    }

    BOOST_FORCEINLINE bool compare_exchange_strong(value_type& expected, value_arg_type desired, memory_order success_order, memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        BOOST_ASSERT(failure_order != memory_order_release);
        BOOST_ASSERT(failure_order != memory_order_acq_rel);
        BOOST_ASSERT(cas_failure_order_must_not_be_stronger_than_success_order(success_order, failure_order));

        return compare_exchange_strong_impl(expected, desired, success_order, failure_order, cxchg_use_bitwise_cast());
    }

    BOOST_FORCEINLINE bool compare_exchange_strong(value_type& expected, value_arg_type desired, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return compare_exchange_strong(expected, desired, order, atomics::detail::deduce_failure_order(order));
    }

    BOOST_FORCEINLINE bool compare_exchange_weak(value_type& expected, value_arg_type desired, memory_order success_order, memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        BOOST_ASSERT(failure_order != memory_order_release);
        BOOST_ASSERT(failure_order != memory_order_acq_rel);
        BOOST_ASSERT(cas_failure_order_must_not_be_stronger_than_success_order(success_order, failure_order));

        return compare_exchange_weak_impl(expected, desired, success_order, failure_order, cxchg_use_bitwise_cast());
    }

    BOOST_FORCEINLINE bool compare_exchange_weak(value_type& expected, value_arg_type desired, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return compare_exchange_weak(expected, desired, order, atomics::detail::deduce_failure_order(order));
    }

    // Boost.Atomic extensions
    BOOST_FORCEINLINE value_type fetch_negate(memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return extra_fp_operations::fetch_negate(this->storage(), order);
    }

    BOOST_FORCEINLINE value_type add(difference_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return extra_fp_operations::add(this->storage(), v, order);
    }

    BOOST_FORCEINLINE value_type sub(difference_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return extra_fp_operations::sub(this->storage(), v, order);
    }

    BOOST_FORCEINLINE value_type negate(memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return extra_fp_operations::negate(this->storage(), order);
    }

    BOOST_FORCEINLINE void opaque_add(difference_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        extra_fp_operations::opaque_add(this->storage(), v, order);
    }

    BOOST_FORCEINLINE void opaque_sub(difference_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        extra_fp_operations::opaque_sub(this->storage(), v, order);
    }

    BOOST_FORCEINLINE void opaque_negate(memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        extra_fp_operations::opaque_negate(this->storage(), order);
    }

    // Operators
    BOOST_FORCEINLINE value_type operator+=(difference_type v) volatile BOOST_NOEXCEPT
    {
        return add(v);
    }

    BOOST_FORCEINLINE value_type operator-=(difference_type v) volatile BOOST_NOEXCEPT
    {
        return sub(v);
    }

    BOOST_FORCEINLINE value_type wait(value_arg_type old_val, memory_order order = memory_order_seq_cst) const volatile BOOST_NOEXCEPT
    {
        BOOST_ASSERT(order != memory_order_release);
        BOOST_ASSERT(order != memory_order_acq_rel);

        return atomics::detail::bitwise_fp_cast< value_type >(wait_operations::wait(this->storage(), atomics::detail::bitwise_fp_cast< storage_type >(old_val), order));
    }

    BOOST_DELETED_FUNCTION(base_atomic(base_atomic const&))
    BOOST_DELETED_FUNCTION(base_atomic& operator=(base_atomic const&))

private:
    BOOST_FORCEINLINE bool compare_exchange_strong_impl(value_type& expected, value_arg_type desired, memory_order success_order, memory_order failure_order, atomics::detail::false_type) volatile BOOST_NOEXCEPT
    {
        return core_operations::compare_exchange_strong(this->storage(), reinterpret_cast< storage_type& >(expected), atomics::detail::bitwise_fp_cast< storage_type >(desired), success_order, failure_order);
    }

    BOOST_FORCEINLINE bool compare_exchange_strong_impl(value_type& expected, value_arg_type desired, memory_order success_order, memory_order failure_order, atomics::detail::true_type) volatile BOOST_NOEXCEPT
    {
        storage_type old_value = atomics::detail::bitwise_fp_cast< storage_type >(expected);
        const bool res = core_operations::compare_exchange_strong(this->storage(), old_value, atomics::detail::bitwise_fp_cast< storage_type >(desired), success_order, failure_order);
        expected = atomics::detail::bitwise_fp_cast< value_type >(old_value);
        return res;
    }

    BOOST_FORCEINLINE bool compare_exchange_weak_impl(value_type& expected, value_arg_type desired, memory_order success_order, memory_order failure_order, atomics::detail::false_type) volatile BOOST_NOEXCEPT
    {
        return core_operations::compare_exchange_weak(this->storage(), reinterpret_cast< storage_type& >(expected), atomics::detail::bitwise_fp_cast< storage_type >(desired), success_order, failure_order);
    }

    BOOST_FORCEINLINE bool compare_exchange_weak_impl(value_type& expected, value_arg_type desired, memory_order success_order, memory_order failure_order, atomics::detail::true_type) volatile BOOST_NOEXCEPT
    {
        storage_type old_value = atomics::detail::bitwise_fp_cast< storage_type >(expected);
        const bool res = core_operations::compare_exchange_weak(this->storage(), old_value, atomics::detail::bitwise_fp_cast< storage_type >(desired), success_order, failure_order);
        expected = atomics::detail::bitwise_fp_cast< value_type >(old_value);
        return res;
    }
};

#endif // !defined(BOOST_ATOMIC_NO_FLOATING_POINT)


//! Implementation for pointers to object types
template< typename T, bool Interprocess >
class base_atomic< T*, void*, Interprocess > :
    public base_atomic_common< T*, false, Interprocess >
{
private:
    typedef base_atomic_common< T*, false, Interprocess > base_type;

public:
    typedef typename base_type::value_type value_type;
    typedef std::ptrdiff_t difference_type;

protected:
    typedef typename base_type::core_operations core_operations;
    typedef typename base_type::wait_operations wait_operations;
    typedef atomics::detail::extra_operations< core_operations > extra_operations;
    typedef typename base_type::storage_type storage_type;
    typedef value_type value_arg_type;

private:
#if !defined(BOOST_ATOMIC_DETAIL_STORAGE_TYPE_MAY_ALIAS) || !defined(BOOST_ATOMIC_NO_CLEAR_PADDING)
    typedef atomics::detail::true_type cxchg_use_bitwise_cast;
#else
    typedef atomics::detail::integral_constant< bool, sizeof(value_type) != sizeof(storage_type) || atomics::detail::alignment_of< value_type >::value <= core_operations::storage_alignment > cxchg_use_bitwise_cast;
#endif

    // uintptr_storage_type is the minimal storage type that is enough to store pointers. The actual storage_type theoretically may be larger,
    // if the target architecture only supports atomic ops on larger data. Typically, though, they are the same type.
    typedef atomics::detail::uintptr_t uintptr_storage_type;

public:
    BOOST_DEFAULTED_FUNCTION(base_atomic() BOOST_ATOMIC_DETAIL_DEF_NOEXCEPT_DECL, BOOST_ATOMIC_DETAIL_DEF_NOEXCEPT_IMPL {})
    BOOST_FORCEINLINE BOOST_ATOMIC_DETAIL_CONSTEXPR_ATOMIC_CTOR explicit base_atomic(value_arg_type v) BOOST_NOEXCEPT :
        base_type(atomics::detail::bitwise_cast< uintptr_storage_type >(v))
    {
    }

    // Standard methods
    BOOST_FORCEINLINE void store(value_arg_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        BOOST_ASSERT(order != memory_order_consume);
        BOOST_ASSERT(order != memory_order_acquire);
        BOOST_ASSERT(order != memory_order_acq_rel);

        core_operations::store(this->storage(), atomics::detail::bitwise_cast< uintptr_storage_type >(v), order);
    }

    BOOST_FORCEINLINE value_type load(memory_order order = memory_order_seq_cst) const volatile BOOST_NOEXCEPT
    {
        BOOST_ASSERT(order != memory_order_release);
        BOOST_ASSERT(order != memory_order_acq_rel);

        return atomics::detail::bitwise_cast< value_type >(static_cast< uintptr_storage_type >(core_operations::load(this->storage(), order)));
    }

    BOOST_FORCEINLINE value_type fetch_add(difference_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return atomics::detail::bitwise_cast< value_type >(static_cast< uintptr_storage_type >(core_operations::fetch_add(this->storage(), static_cast< uintptr_storage_type >(v * sizeof(T)), order)));
    }

    BOOST_FORCEINLINE value_type fetch_sub(difference_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return atomics::detail::bitwise_cast< value_type >(static_cast< uintptr_storage_type >(core_operations::fetch_sub(this->storage(), static_cast< uintptr_storage_type >(v * sizeof(T)), order)));
    }

    BOOST_FORCEINLINE value_type exchange(value_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return atomics::detail::bitwise_cast< value_type >(static_cast< uintptr_storage_type >(core_operations::exchange(this->storage(), atomics::detail::bitwise_cast< uintptr_storage_type >(v), order)));
    }

    BOOST_FORCEINLINE bool compare_exchange_strong(value_type& expected, value_arg_type desired, memory_order success_order, memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        BOOST_ASSERT(failure_order != memory_order_release);
        BOOST_ASSERT(failure_order != memory_order_acq_rel);
        BOOST_ASSERT(cas_failure_order_must_not_be_stronger_than_success_order(success_order, failure_order));

        return compare_exchange_strong_impl(expected, desired, success_order, failure_order, cxchg_use_bitwise_cast());
    }

    BOOST_FORCEINLINE bool compare_exchange_strong(value_type& expected, value_arg_type desired, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return compare_exchange_strong(expected, desired, order, atomics::detail::deduce_failure_order(order));
    }

    BOOST_FORCEINLINE bool compare_exchange_weak(value_type& expected, value_arg_type desired, memory_order success_order, memory_order failure_order) volatile BOOST_NOEXCEPT
    {
        BOOST_ASSERT(failure_order != memory_order_release);
        BOOST_ASSERT(failure_order != memory_order_acq_rel);
        BOOST_ASSERT(cas_failure_order_must_not_be_stronger_than_success_order(success_order, failure_order));

        return compare_exchange_weak_impl(expected, desired, success_order, failure_order, cxchg_use_bitwise_cast());
    }

    BOOST_FORCEINLINE bool compare_exchange_weak(value_type& expected, value_arg_type desired, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return compare_exchange_weak(expected, desired, order, atomics::detail::deduce_failure_order(order));
    }

    // Boost.Atomic extensions
    BOOST_FORCEINLINE value_type add(difference_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return atomics::detail::bitwise_cast< value_type >(static_cast< uintptr_storage_type >(extra_operations::add(this->storage(), static_cast< uintptr_storage_type >(v * sizeof(T)), order)));
    }

    BOOST_FORCEINLINE value_type sub(difference_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return atomics::detail::bitwise_cast< value_type >(static_cast< uintptr_storage_type >(extra_operations::sub(this->storage(), static_cast< uintptr_storage_type >(v * sizeof(T)), order)));
    }

    BOOST_FORCEINLINE void opaque_add(difference_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        extra_operations::opaque_add(this->storage(), static_cast< uintptr_storage_type >(v * sizeof(T)), order);
    }

    BOOST_FORCEINLINE void opaque_sub(difference_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        extra_operations::opaque_sub(this->storage(), static_cast< uintptr_storage_type >(v * sizeof(T)), order);
    }

    BOOST_FORCEINLINE bool add_and_test(difference_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return extra_operations::add_and_test(this->storage(), static_cast< uintptr_storage_type >(v * sizeof(T)), order);
    }

    BOOST_FORCEINLINE bool sub_and_test(difference_type v, memory_order order = memory_order_seq_cst) volatile BOOST_NOEXCEPT
    {
        return extra_operations::sub_and_test(this->storage(), static_cast< uintptr_storage_type >(v * sizeof(T)), order);
    }

    // Operators
    BOOST_FORCEINLINE value_type operator++(int) volatile BOOST_NOEXCEPT
    {
        return fetch_add(1);
    }

    BOOST_FORCEINLINE value_type operator++() volatile BOOST_NOEXCEPT
    {
        return add(1);
    }

    BOOST_FORCEINLINE value_type operator--(int) volatile BOOST_NOEXCEPT
    {
        return fetch_sub(1);
    }

    BOOST_FORCEINLINE value_type operator--() volatile BOOST_NOEXCEPT
    {
        return sub(1);
    }

    BOOST_FORCEINLINE value_type operator+=(difference_type v) volatile BOOST_NOEXCEPT
    {
        return add(v);
    }

    BOOST_FORCEINLINE value_type operator-=(difference_type v) volatile BOOST_NOEXCEPT
    {
        return sub(v);
    }

    BOOST_FORCEINLINE value_type wait(value_arg_type old_val, memory_order order = memory_order_seq_cst) const volatile BOOST_NOEXCEPT
    {
        BOOST_ASSERT(order != memory_order_release);
        BOOST_ASSERT(order != memory_order_acq_rel);

        return atomics::detail::bitwise_cast< value_type >(static_cast< uintptr_storage_type >(wait_operations::wait(this->storage(), atomics::detail::bitwise_cast< uintptr_storage_type >(old_val), order)));
    }

    BOOST_DELETED_FUNCTION(base_atomic(base_atomic const&))
    BOOST_DELETED_FUNCTION(base_atomic& operator=(base_atomic const&))

private:
    BOOST_FORCEINLINE bool compare_exchange_strong_impl(value_type& expected, value_arg_type desired, memory_order success_order, memory_order failure_order, atomics::detail::false_type) volatile BOOST_NOEXCEPT
    {
        return core_operations::compare_exchange_strong(this->storage(), reinterpret_cast< storage_type& >(expected), atomics::detail::bitwise_cast< uintptr_storage_type >(desired), success_order, failure_order);
    }

    BOOST_FORCEINLINE bool compare_exchange_strong_impl(value_type& expected, value_arg_type desired, memory_order success_order, memory_order failure_order, atomics::detail::true_type) volatile BOOST_NOEXCEPT
    {
        storage_type old_value = atomics::detail::bitwise_cast< uintptr_storage_type >(expected);
        const bool res = core_operations::compare_exchange_strong(this->storage(), old_value, atomics::detail::bitwise_cast< uintptr_storage_type >(desired), success_order, failure_order);
        expected = atomics::detail::bitwise_cast< value_type >(static_cast< uintptr_storage_type >(old_value));
        return res;
    }

    BOOST_FORCEINLINE bool compare_exchange_weak_impl(value_type& expected, value_arg_type desired, memory_order success_order, memory_order failure_order, atomics::detail::false_type) volatile BOOST_NOEXCEPT
    {
        return core_operations::compare_exchange_weak(this->storage(), reinterpret_cast< storage_type& >(expected), atomics::detail::bitwise_cast< uintptr_storage_type >(desired), success_order, failure_order);
    }

    BOOST_FORCEINLINE bool compare_exchange_weak_impl(value_type& expected, value_arg_type desired, memory_order success_order, memory_order failure_order, atomics::detail::true_type) volatile BOOST_NOEXCEPT
    {
        storage_type old_value = atomics::detail::bitwise_cast< uintptr_storage_type >(expected);
        const bool res = core_operations::compare_exchange_weak(this->storage(), old_value, atomics::detail::bitwise_cast< uintptr_storage_type >(desired), success_order, failure_order);
        expected = atomics::detail::bitwise_cast< value_type >(static_cast< uintptr_storage_type >(old_value));
        return res;
    }
};

} // namespace detail
} // namespace atomics
} // namespace boost

#include <boost/atomic/detail/footer.hpp>

#endif // BOOST_ATOMIC_DETAIL_ATOMIC_IMPl_HPP_INCLUDED_
